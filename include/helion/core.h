// [License]
// MIT - See LICENSE.md file in the package.

#pragma once

#ifndef __HELION_CORE_JIT__
#define __HELION_CORE_JIT__

#include <memory>
#include <vector>



#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"

// All the IR libs that are needed
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
// Various transformations
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Vectorize.h"

#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"



#include <memory>
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <flat_hash_map.hpp>
#include <mutex>

#include <helion/text.h>

/*
 * this header file defines the core classes and data types used throughout the
 * helion compiler and jit runtime. For example, the module type, the basic type
 * classes, etc...
 */

namespace helion {


  namespace ast {
    class node;
    class typedef_node;
    class module;
    class type_node;

    class def;
    class func;
  };  // namespace ast

  extern llvm::LLVMContext llvm_ctx;

  struct binding;
  struct datatype_name;
  struct datatype;


  // a linkage to the 'Any' type
  extern datatype *any_type;
  // a linkage to the basic int type
  extern datatype *int32_type;
  // a linkage to the basic float type
  extern datatype *float32_type;


  // a value is an opaque pointer to something garbage collected in the helion
  // jit runtime. It has no real meaning except as a better, typed replacement
  // for voidptr
  struct value;

  struct datatype_name {};
  enum class type_style : unsigned char {
    OBJECT,    // normal reference type
    INTEGER,   // is an n bit integer
    FLOATING,  // is an n bit floating point number
    UNION,     // is a union of the parameters
    TUPLE,
    METHOD,    // first parameter is the return type, then each other parameter
               // is an argument
    SLICE,     // the first parameter is the type of the slice. Only one param
               // allowed
    OPTIONAL,  // first param is the type it is an optional of
  };



  struct typeinfo {
    // a type can have multiple 'styles'. For example, Int32 has the style of
    // INTEGER, and thne has a size of 32. This is helpful when lowering to
    // llvm::Type

    std::shared_ptr<ast::typedef_node> node;

    type_style style = type_style::OBJECT;
    std::string name;

    // how many bits this type is in memory (for primitive types)
    int bits;

    // a type is specialized iff it's parameters are filled in correctly
    bool specialized = false;

    // the super type of this type. Defaults to any_type
    datatype *super;

    std::vector<std::string> param_names;

    std::vector<std::unique_ptr<datatype>> specializations;

    std::mutex lock;
  };




  // the datatype representation in the engine
  struct datatype {
    struct field {
      datatype *type;
      std::string name;
    };
    std::shared_ptr<typeinfo> ti;


    bool specialized = false;
    bool completed = false;
    // a list of type parameters. ie: Vector<Int>
    std::vector<datatype *> param_types;
    // declaration of the type in LLVM as an llvm::Type
    llvm::Type *type_decl = nullptr;
    std::vector<field> fields;

    static datatype &create(std::string, datatype & = *any_type,
                            std::vector<std::string> = {});
    static inline datatype &create(std::string n, std::vector<std::string> p) {
      return datatype::create(n, *any_type, p);
    };

    static datatype &create_integer(std::string, int);
    static datatype &create_float(std::string, int);

    void add_field(std::string, datatype *);
    // datatype *specialize(std::vector<datatype *>);

    llvm::Type *to_llvm(void);

    text str(void);

    inline datatype *spawn_spec() {
      auto n = new datatype(*this);
      n->specialized = true;
      ti->specializations.push_back(std::unique_ptr<datatype>(n));
      return n;
    }

   private:
    inline datatype(std::string name, datatype &s) {
      ti = std::make_shared<typeinfo>();
      ti->name = name;
      ti->super = &s;
    }

    inline datatype(datatype &other) { ti = other.ti; }
  };




  class pattern_match_error : public std::exception {
    std::string _msg;

   public:
    long line;
    long col;

    pattern_match_error(ast::type_node &n, datatype &with, std::string msg);
    inline const char *what() const throw() {
      // simply pull the value out of the msg
      return _msg.c_str();
    }
  };



  struct cgval {
    llvm::Value *v;
    datatype *typ;
    cgval(llvm::Value *v, datatype *typ) : v(v), typ(typ) {}
    cgval() = delete;
    inline operator llvm::Value *() const { return v; }
    inline operator datatype *() const { return typ; }
  };



  // forward decl of codegen structs
  class cg_ctx;
  struct cg_binding;
  class cg_scope;


  datatype *specialize(std::shared_ptr<ast::type_node> &, cg_scope *);
  datatype *specialize(datatype *, std::vector<datatype *>, cg_scope *);
  datatype *specialize(datatype *, cg_scope *);


  // attempt to pattern match a type on another type, possibly updating the
  // scope with the parameter type names
  void pattern_match(std::shared_ptr<ast::type_node> &, datatype *, cg_scope *);



  // check if two types are
  bool subtype(datatype *A, datatype *B);


  /**
   * A method signature represents the type of a signature at runtime. It is
   * used to represent return types and argument types. Each method_signature is
   * stored and owned by a static map in `method.cpp`, and method signatures are
   * handled by an int64. The reason for this abstraction is because at runtime,
   * there needs to be efficient lookup of these method signatures in methods
   * because lambdas should have specializations compiled at runtime as they are
   * needed. This is not really a problem for top level defs, as we can
   * statically determine this information
   */
  struct method_signature {
    datatype *return_type;
    std::vector<datatype *> arguments;
  };

  class method_instance;

  class method {
   public:
    using sig_handle = int64_t;
    cg_scope *scope;
    std::string name;
    std::string file;
    std::shared_ptr<ast::node> src;
    // table of all method_instance specializations that we've compiled
    std::vector<method_instance *> specializations;

    static method *create(std::shared_ptr<ast::def> &);
    static method *create(std::shared_ptr<ast::func> &, cg_scope *);


    // a simple list of the ast nodes that define entry points to this method
    // for example, if a function is defined more than once, each of the
    // overloads go into this vector. When an implementation is needed at
    // compile time, the compiler will go through this list to find a best-fit.
    // Unfortunately, this means there could be ambiguity in choosing a method.
    std::vector<std::shared_ptr<ast::func>> definitions;

   private:
    std::mutex lock;
    // the
    ska::flat_hash_map<method::sig_handle, method_instance *> instances;
  };


  class method_instance {
   public:
    // what method is this an instance of?
    method *of;
  };


  using RTDyldObjHandleT = llvm::orc::VModuleKey;

  // orc jit execution engine
  // see julia's src/jitlayer.h for reference.
  // (most of the JIT design is stolen from julia's :>)
  class ojit_ee {
   public:
    using CompilerResultT = std::unique_ptr<llvm::MemoryBuffer>;
    typedef llvm::orc::LegacyRTDyldObjectLinkingLayer ObjLayer;
    typedef llvm::orc::LegacyIRCompileLayer<ObjLayer, llvm::orc::SimpleCompiler>
        CompileLayer;
    typedef llvm::orc::VModuleKey ModuleHandle;
    typedef llvm::StringMap<void *> SymbolTableT;
    typedef llvm::object::OwningBinary<llvm::object::ObjectFile> OwningObj;




    ojit_ee(llvm::TargetMachine &TM);


    void add_global_mapping(llvm::StringRef, uint64_t);



    ModuleHandle add_module(std::unique_ptr<llvm::Module>);
    void remove_module(ModuleHandle);

    const llvm::DataLayout &getDataLayout() const;
    const llvm::Triple &getTargetTriple() const;

    inline llvm::JITSymbol find_symbol(const std::string name) {
      return find_mangled_symbol(mangle(name));
    }

    inline uint64_t get_type_size(llvm::Type *t) {
      return DL.getTypeAllocSize(t);
    }

    void *get_function_address(std::string);

    inline void add_dlhandle(void *h) { dlhandles.push_back(h); }

   private:
    inline std::string mangle(const std::string &Name) {
      std::string MangledName;
      {
        llvm::raw_string_ostream MangledNameStream(MangledName);
        llvm::Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
      }
      return MangledName;
    }


    std::unique_ptr<llvm::Module> opt_module(std::unique_ptr<llvm::Module>);



    llvm::JITSymbol find_mangled_symbol(const std::string &Name,
                                        bool ExportedSymbolsOnly = false);
    llvm::TargetMachine &TM;
    const llvm::DataLayout DL;
    // Should be big enough that in the common case, The
    // object fits in its entirety

    llvm::orc::ExecutionSession exec_session;
    std::shared_ptr<llvm::orc::SymbolResolver> symbol_resolver;
    std::shared_ptr<llvm::RTDyldMemoryManager> mem_mgr;
    ObjLayer obj_layer;
    CompileLayer compile_layer;

    using OptimizeFunction = std::function<std::unique_ptr<llvm::Module>(
        std::unique_ptr<llvm::Module>)>;


    llvm::orc::LegacyIRTransformLayer<CompileLayer, OptimizeFunction> opt_layer;

    SymbolTableT GlobalSymbolTable;
    SymbolTableT LocalSymbolTable;
    std::vector<ModuleHandle> module_keys;

    std::vector<void *> dlhandles;
  };




  // execution engine is a nice global that exposes interfaces that
  // make sense for the use-cases of helion
  extern ojit_ee *execution_engine;


  // a global_variable is what helion global variables are stored in
  struct global_variable {
    datatype *type;
    text name;
    // A pointer to an unknown size of memory. Allocated
    // when the global variable is created. It is enough to store
    // a variable of the type in. ie: with objects, it's large enough
    // to store a pointer. With primitives, its how many bytes that primitive
    // is.
    void *data;

    ~global_variable();
  };


  class module {
    // module *parent = nullptr;
    text name;
    std::map<std::string, std::unique_ptr<global_variable>> globals;

   public:
    // represents the global scope for this module
    std::unique_ptr<cg_scope> scope;

    global_variable *find(std::string);

    // Returns a pointer to the cell which the value is stored in
    void *global_create(std::string, datatype *);
  };



  void compile_module(std::unique_ptr<ast::module> m);


  void init_types(void);
  void init_codegen(void);


  inline void init() {
    // initialize the types before the codegen, because the codegen will require
    // the the builtin types.
    init_types();
    init_codegen();
  }




  void *global_find(std::string);
  void *global_set(std::string);

};  // namespace helion

#endif
