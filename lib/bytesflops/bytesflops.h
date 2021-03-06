/*
 * Instrument code to keep track of run-time behavior:
 * LLVM pass class definition
 *
 * By Scott Pakin <pakin@lanl.gov>
 *    Pat McCormick <pat@lanl.gov>
 *    Rob Aulwes <rta@lanl.gov>
 */

#ifndef BYTES_FLOPS_H_
#define BYTES_FLOPS_H_

#include "llvm/ADT/StringMap.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <memory>
#include <set>
#include <iomanip>
#include <unordered_map>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include "byfl-common.h"
#include "functionkeygen.h"

using namespace std;
using namespace llvm;

namespace bytesflops_pass {

  // Define a command-line option for outputting results at the end of
  // every basic block instead of only once at the end of the program.
  extern cl::opt<bool> InstrumentEveryBB;

  // Define a command-line option for aggregating measurements by
  // function name.
  extern cl::opt<bool> TallyByFunction;

  // Define a command-line option for outputting not only function
  // names but also immediate parents.
  extern cl::opt<bool> TrackCallStack;

  // Define a command-line option for keeping track of unique bytes.
  extern cl::opt<bool> TrackUniqueBytes;

  // Define a command-line option for helping find a program's
  // working-set size.
  extern cl::opt<bool> FindMemFootprint;

  // Define a command-line option for tallying loads and stored by
  // data structure.
  extern cl::opt<bool> TallyByDataStruct;

  // Define a command-line option for tallying load/store operations
  // based on various data types.
  extern cl::opt<bool> TallyTypes;

  // Define a command-line option for tallying a histogram of the
  // occurrence of individual instructions within the code; aka the
  // instruction mix.
  extern cl::opt<bool> TallyInstMix;

  // Define a command-line option for tallying instruction dependencies.
  extern cl::opt<bool> TallyInstDeps;

  // Define a command-line option for merging basic-block measurements
  // to reduce the output volume.
  extern cl::opt<unsigned long long> BBMergeCount;

  // Define a command-line option to accept a list of functions to
  // instrument, ignoring all others.
  extern cl::list<string> IncludedFunctions;

  // Define a command-line option to accept a list of functions not to
  // instrument, including all others.
  extern cl::list<string> ExcludedFunctions;

  // Define a command-line option for enabling thread safety (at the
  // cost of increasing execution time).
  extern cl::opt<bool> ThreadSafety;

  // Define a command-line option for tallying vector operations.
  extern cl::opt<bool> TallyVectors;

  // Define a command-line option for tracking reuse distance.
  typedef enum {RD_LOADS, RD_STORES, RD_BOTH} ReuseDistType;
  extern cl::bits<ReuseDistType> ReuseDist;
  extern unsigned int rd_bits;    // Same as ReuseDist.getBits() but with RD_BOTH expanded

  // Define a command-line option for pruning reuse distance.
  extern cl::opt<unsigned long long> MaxReuseDist;

  // Define a command-line option for turning on the cache model.
  extern cl::opt<bool> CacheModel;

  // Define a command-line option for cache line size in bytes.
  extern cl::opt<unsigned long long> CacheLineBytes;

  // Define a command-line option for log2 of the maximum number of sets to model.
  extern cl::opt<unsigned long long> CacheMaxSetBits;

  // Define a command-line option for tracking load/store strides.
  extern cl::opt<bool> TrackStrides;

  // Destructively remove all instances of a given character from a string.
  extern void remove_all_instances(string& some_string, char some_char);

  // Parse a list of function names into a set.  The trick is that
  // demangled C++ function names are split (at commas) across list
  // elements and need to be recombined.
  extern set<string>* parse_function_names(vector<string>& funclist);

  // Define a class version of bf_symbol_info_t for internal use.
  class InternalSymbolInfo {
  private:
    // Map a Function to a {file, line} tuple.
    typedef pair<string, unsigned int> string_uint_pair;
    static unordered_map<const Function*, string_uint_pair> *func2loc;

    // Populate func2loc with every function in the module.
    void initialize_func2loc (const Module* module);

    // Indicate whether all fields have been assigned a non-default value.
    bool have_all_fields();

    // Randomly generate IDs.
    static MersenneTwister *prng;

  public:
    uint64_t ID;           // Unique identifier for the symbol
    string origin;         // Who allocate the symbol
    string symbol;         // Symbol name
    string function;       // Name of function containing the symbol
    string file;           // Name of directory+file containing the symbol
    unsigned int line;     // Line number at which the symbol appears
    bool precise;          // true=precise source location; false=approximate

    // Read the metadata associated with a value and construct an
    // InternalSymbolInfo that represents the value's provenance.
    InternalSymbolInfo(Value* value, string defn_loc);

    // Construct an InternalSymbolInfo directly from a DIGlobalVariable..
    InternalSymbolInfo(DIGlobalVariable& var, string defn_loc);

    // Construct an InternalSymbolInfo from a Function.
    InternalSymbolInfo(Function* funcptr);

  // Format our contents and write them to a stream.
    friend raw_ostream& operator<< (raw_ostream &ros, const InternalSymbolInfo& sym) {
      ros << sym.function << ':' << sym.symbol << " @ " << sym.file << ':' << sym.line;
      return ros;
    }
  };

  // Define a pass over each basic block in the module.
  //  struct BytesFlops : public FunctionPass {
  struct BytesFlops : public ModulePass {

  private:
    static const int CLEAR_LOADS;
    static const int CLEAR_FLOAT_LOADS;
    static const int CLEAR_DOUBLE_LOADS;
    static const int CLEAR_INT_LOADS;
    static const int CLEAR_PTR_LOADS;
    static const int CLEAR_OTHER_TYPE_LOADS;

    static const int CLEAR_STORES;
    static const int CLEAR_FLOAT_STORES;
    static const int CLEAR_DOUBLE_STORES;
    static const int CLEAR_INT_STORES;
    static const int CLEAR_PTR_STORES;
    static const int CLEAR_OTHER_TYPE_STORES;

    static const int CLEAR_FLOPS;
    static const int CLEAR_FP_BITS;
    static const int CLEAR_OPS;
    static const int CLEAR_OP_BITS;

    static const int CLEAR_CALLS;
    static const int CLEAR_MEM_TYPES;

    GlobalVariable* load_var;  // Global reference to bf_load_count, a 64-bit load counter
    GlobalVariable* store_var; // Global reference to bf_store_count, a 64-bit store counter
    GlobalVariable* load_inst_var;              // Global reference to bf_load_ins_count, a 64-bit load-instruction counter
    GlobalVariable* store_inst_var;             // Global reference to bf_store_ins_count, a 64-bit store-instruction counter
    GlobalVariable* mem_insts_var;              // Global reference to bf_mem_insts, a set of 64-bit memory instruction counters
    GlobalVariable* inst_mix_histo_var;         // Global reference to bf_inst_mix_histo, an array representing a histogram of specific instruction counts.
    GlobalVariable* inst_deps_histo_var;        // Global reference to bf_inst_deps_histo, an array representing histogram of instruction dependencies.
    GlobalVariable* terminator_var;             // Global reference to bf_terminator_count, an array of terminator tallies
    GlobalVariable* mem_intrinsics_var;         // Global reference to bf_mem_intrin_count, tallies of memory intrinsics
    GlobalVariable* flop_var;  // Global reference to bf_flop_count, a 64-bit flop counter
    GlobalVariable* fp_bits_var;  // Global reference to bf_fp_bits_count, a 64-bit FP-bit counter
    GlobalVariable* op_var;    // Global reference to bf_op_count, a 64-bit operation counter
    GlobalVariable* op_bits_var;   // Global reference to bf_op_bits_count, a 64-bit operation-bit counter
    GlobalVariable* call_inst_var;              // Global reference to bf_call_ins_count, a 64-bit call-instruction counter
    uint64_t static_loads;   // Number of static load instructions
    uint64_t static_stores;  // Number of static store instructions
    uint64_t static_flops;   // Number of static floating-point instructions
    uint64_t static_ops;     // Number of static instructions of any type (except no-ops)
    uint64_t static_cond_brs;  // Number of static conditional or indirect branch instructions
    uint64_t static_bblocks;   // Number of static basic blocks
    Function* init_func_map;
    Function* init_if_necessary;  // Pointer to bf_initialize_if_necessary()
    Function* accum_bb_tallies;   // Pointer to bf_accumulate_bb_tallies()
    Function* report_bb_tallies;  // Pointer to bf_report_bb_tallies()
    Function* reset_bb_tallies;   // Pointer to bf_reset_bb_tallies()
    Function* assoc_counts_with_func;    // Pointer to bf_assoc_counters_with_func()
    Function* assoc_addrs_with_func;    // Pointer to bf_assoc_addresses_with_func()
    Function* assoc_addrs_with_prog;    // Pointer to bf_assoc_addresses_with_prog()
    Function* push_function;       // Pointer to bf_push_function()
    Function* pop_function;        // Pointer to bf_pop_function()
    Function* tally_function;      // Pointer to bf_incr_func_tally()
    Function* take_mega_lock;      // Pointer to bf_acquire_mega_lock()
    Function* release_mega_lock;   // Pointer to bf_release_mega_lock()
    Function* tally_vector;        // Pointer to bf_tally_vector_operation()
    Function* access_data_struct;  // Pointer to bf_access_data_struct()
    Function* assoc_addrs_with_sstruct;     // Pointer to bf_assoc_addresses_with_sstruct()
    Function* assoc_addrs_with_dstruct;     // Pointer to bf_assoc_addresses_with_dstruct
    Function* assoc_addrs_with_dstruct_pm;  // Pointer to bf_assoc_addresses_with_dstruct_pm
    Function* assoc_addrs_with_dstruct_stack;  // Pointer to bf_assoc_addresses_with_dstruct_stack
    Function* disassoc_addrs_with_dstruct;  // Pointer to bf_disassoc_addresses_with_dstruct
    Function* reuse_dist_prog;   // Pointer to bf_reuse_dist_addrs_prog()
    Function* memset_intrinsic;  // Pointer to LLVM's memset() intrinsic
    Function* access_cache;      // Pointer to bf_touch_cache()
    Function* tally_bb_exec;     // Pointer to bf_tally_bb_execution()
    Function* track_stride;      // Pointer to bf_track_stride()
    StringMap<Constant*> func_name_to_arg;   // Map from a function name to an IR function argument
    set<string>* instrument_only;   // Set of functions to instrument; NULL=all
    set<string>* dont_instrument;   // Set of functions not to instrument; NULL=none
    ConstantInt* not_end_of_bb;     // 0, not at the end of a basic block
    ConstantInt* uncond_end_bb;     // 1, basic block ended with an unconditional branch
    ConstantInt* cond_end_bb;       // 2, basic block ended with a conditional branch
    ConstantInt* zero;        // A 64-bit constant "0"
    ConstantInt* one;         // A 64-bit constant "1"
    ConstantPointerNull* null_pointer;           // (void*)NULL
    ConstantPointerNull* null_syminfo_pointer;   // (bf_symbol_info_t*)NULL
    typedef unordered_map<string, unsigned long> str2ul_t;
    str2ul_t loop_len;        // Number of instructions in each inner loop
    StructType* syminfo_type; // bf_symbol_info_t struct type
    AllocaInst* func_syminfo;   // Recyclable, function-local, stack-allocated bf_symbol_info_t struct

    // Say whether one str2ul_t should be output before another.
    class compare_str2ul_t {
    private:
      const str2ul_t* loop_len;
    public:
      // Say whether one string in loop_len should be output before another.
      bool operator() (const string& s1, const string& s2) const {
        unsigned long len1 = loop_len->at(s1);
        unsigned long len2 = loop_len->at(s2);
        if (len1 == len2)
          return s1 < s2;
        else
          return len1 > len2;
      }
      compare_str2ul_t (const str2ul_t& ll) {
        loop_len = &ll;
      }
    };

    Function* record_funcs2keys;     // Pointer to bf_record_funcs2keys()
    Function* func_map_ctor;  // static constructor for the function keys
    std::unique_ptr<FunctionKeyGen>    m_keygen;
    std::map<std::string, KeyType_t>   func_key_map;
    std::vector<KeyType_t>             recorded;

    GlobalVariable * byfl_fmap_cnt;

    // Mark an instruction as having been produced by Byfl.
    void mark_as_byfl(Instruction* inst);

    // Insert after a given instruction some code to increment a
    // global variable.
    void increment_global_variable(BasicBlock::iterator& iter,
                                   Constant* global_var,
                                   Value* increment);

    // Insert after a given instruction some code to increment an
    // element of a global array.
    void increment_global_array(BasicBlock::iterator& insert_before,
                                Constant* global_var,
                                Value* idx,
                                Value* increment);

    // Insert before a given instruction some code to increment an element of a
    // global 4-D array.
    void increment_global_4D_array(BasicBlock::iterator& insert_before,
                                   GlobalVariable* global_var,
                                   Value* idx1,
                                   Value* idx2,
                                   Value* idx3,
                                   Value* idx4,
                                   Value* increment);

    // Mark a variable as "used" (not eligible for dead-code elimination).
    void mark_as_used(Module& module, Constant* protected_var);

    // Create and initialize a global variable in the
    // instrumented code.
    GlobalVariable* create_global_variable(Module& module,
                                           Type* var_type,
                                           Constant * init_value,
                                           const char* name);

    // Create and initialize a global uint64_t constant in the
    // instrumented code.
    GlobalVariable* create_global_constant(Module& module, const char* name,
                                           uint64_t value, bool reuse_old=false);

    // Create and initialize a global bool constant in the
    // instrumented code.
    GlobalVariable* create_global_constant(Module& module, const char* name,
                                           bool value, bool reuse_old=false);

    // Create and initialize a global char* constant in the
    // instrumented code.
    Constant* create_global_constant(Module& module, const char* name,
                                     const char* value, bool reuse_old=false);

    // Return the number of elements in a given vector.
    ConstantInt* get_vector_length(LLVMContext& bbctx, const Type* dataType,
                                   ConstantInt* scalarValue);


    // Return true if and only if the given instruction should be
    // treated as a do-nothing operation.
    bool is_no_op(const Instruction& inst);

    // Return true if and only if the given instruction should be
    // tallied as a floating-point operation.
    bool is_fp_operation(const Instruction& inst, const unsigned int opcode,
                         const Type* instType);

    // Return the total number of bits consumed and produced by a
    // given instruction.  The result is are a bit unintuitive for
    // certain types of instructions so use with caution.
    uint64_t instruction_operand_bits(const Instruction& inst);


    // Declare a function with external linkage and C calling conventions.
    Function* declare_extern_c(FunctionType *return_type,
                               StringRef func_name, Module *module);

    // Declare a function that takes no arguments and returns no value.
    Function* declare_thunk(Module* module, const char* thunk_name);

    // Map a function name (string) to an argument to an IR function call.
    Constant* map_func_name_to_arg (Module* module, StringRef funcname);

    // Declare an external variable.
    GlobalVariable* declare_global_var(Module& module, Type* var_type,
                                       StringRef var_name, bool is_const=false);

    GlobalVariable* create_global_var(Module& module,
                                      Type* var_type,
                                      StringRef var_name,
                                      size_t nelts);

    // Insert code to set every element of a given array to zero.
    void insert_zero_array_code(Module* module,
                                GlobalVariable* array_to_zero,
                                uint64_t num_elts,
                                BasicBlock::iterator& insert_before);

    // Insert code at the end of a basic block.
    void insert_end_bb_code (Module* module, KeyType_t funcKey, uint64_t num_insts,
                             int& must_clear, BasicBlock::iterator& insert_before);

    // Wrap CallInst::Create() with code to acquire and release the
    // mega-lock when instrumenting in thread-safe mode.
    void callinst_create(Value* function, ArrayRef<Value*> args,
                         Instruction* insert_before);

    // Ditto the above but for parameterless functions.
    void callinst_create(Value* function, Instruction* insert_before);

    // Ditto the above but with a different parameter list.
    void callinst_create(Value* function, BasicBlock* insert_before);

    // Ditto the above but for functions with arguments.
    void callinst_create(Value* function, ArrayRef<Value*> args,
                         BasicBlock* insert_before);

    // Given a Call instruction, return true if we can safely ignore it.
    bool ignorable_call (const Instruction* inst);

    // Tally the number of "real" instructions in a basic block.
    size_t bb_size(const BasicBlock& bb);

    // Convert an LLVM Instruction* to an STL string.
    unordered_map<Instruction*, string> instruction_to_string;
    void map_instructions_to_strings(Function& function);
    string inst_to_string(Instruction* inst);

    // Instrument Load and Store instructions.
    void instrument_load_store(Module* module,
                               StringRef function_name,
                               BasicBlock::iterator& iter,
                               LLVMContext& bbctx,
                               BasicBlock::iterator& terminator_inst,
                               int& must_clear);

    // Instrument Call instructions.
    void instrument_call(Module* module,
                         BasicBlock::iterator& iter,
                         BasicBlock::iterator& insert_before,
                         int& must_clear);

    // Instrument Invoke instructions.
    void instrument_invoke(Module* module,
                           Instruction* inst,
                           BasicBlock::iterator& insert_before,
                           int& must_clear);

    // Instrument Alloca instructions.
    void instrument_alloca(Module* module,
                           StringRef function_name,
                           BasicBlock::iterator& iter,
                           LLVMContext& bbctx,
                           BasicBlock::iterator& terminator_inst);

    // Instrument all instructions.
    void instrument_all(Instruction& iter,
                        LLVMContext& bbctx,
                        BasicBlock::iterator& insert_before,
                        int& must_clear);

    // Instrument miscellaneous instructions.
    void instrument_other(Module* module,
                          StringRef function_name,
                          Instruction& iter,
                          LLVMContext& bbctx,
                          BasicBlock::iterator& insert_before,
                          int& must_clear);

    // Instrument inner loops given a basic block belonging to the loop.
    void instrument_inner_loop(BasicBlock& bb);

    // Do most of the instrumentation work: Walk each instruction in
    // each basic block and add instrumentation code around loads,
    // stores, flops, etc.
    void instrument_entire_function(Module* module, Function& function,
                                    StringRef function_name);

    // Instrument the current basic block iterator (representing a
    // load) for type-specific memory operations.
    void instrument_mem_type(Module* module,
                             bool is_store,
                             BasicBlock::iterator &iter,
                             Type *data_type);

    // Instrument the current basic block iterator (representing a
    // load) for type-specific characteristics.
    void instrument_load_types(BasicBlock::iterator &iter,
                               Type *data_type,
                               int &must_clear);

    // Instrument the current basic block iterator (representing a
    // store) for type-specific characteristics.
    void instrument_store_types(BasicBlock::iterator &iter,
                                Type *data_type,
                                int &must_clear);

    // Keep track of static inner-loop information.
    typedef struct {
      uint64_t basic_blocks;   // Number of basic blocks in the loop
      uint64_t instructions;   // Number of instructions in the loop
    } inner_loop_info_t;
    unordered_map<string, inner_loop_info_t*> loc_to_loop_info;

    // Manage the compile-time hashing of function names.
    void initializeKeyMap(Module* module);
    void create_func_map_ctor(Module& module, uint32_t nkeys,
                              Constant* keys, Constant* fnames);

    // Track all global variable declarations.
    void track_global_variables(Module* module);

    // Read the metadata associated with a value and generate code to construct
    // a bf_symbol_info_t representing where the value came from.
    AllocaInst* find_value_provenance(Module& module, Value* value,
                                      string defn_loc,
                                      BasicBlock::iterator& insert_before,
                                      AllocaInst* syminfo_struct = nullptr);

    // Do the same, but take an InternalSymbolInfo instead of a Value.
    AllocaInst* find_value_provenance(Module& module, InternalSymbolInfo& syminfo,
                                      Instruction* insert_before,
                                      AllocaInst* syminfo_struct = nullptr);

    // Do the same, but take a BasicBlock iterator instead of a Value.
    AllocaInst* find_value_provenance(Module& module,
                                      BasicBlock::iterator& inst_iter,
                                      string defn_loc,
                                      BasicBlock::iterator& insert_before,
                                      AllocaInst* syminfo_struct);

    // Return true if a load or store instruction provably accesses the same
    // addresses on every invocation.
    bool all_constant_refs(Instruction* inst);

  public:
    static char ID;

    BytesFlops() : ModulePass(ID) { }

    const std::map<std::string, KeyType_t> &
      getFuncKeyMap() const {return func_key_map;}

    FunctionKeyGen::KeyID record_func(const std::string & fname);

    // Initialize the BytesFlops pass.
    virtual bool doInitialization(Module& module);

    // Insert code for incrementing our byte, flop, etc. counters.
    virtual bool runOnFunction(Function& function);

    virtual bool runOnModule(Module & module);

    // Insert code for incrementing our byte, flop, etc. counters.
    virtual bool doFinalization(Module& module);

    // Output what we instrumented.
    virtual void print(raw_ostream &outfile, const Module *module) const;
  };

}  // namespace bytesflops_pass


#endif   //BYTES_FLOPS_H_
