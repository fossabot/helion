// [License]
// MIT - See LICENSE.md file in the package.

#include <gc/gc.h>
#include <helion.h>
#include <stdio.h>
#include <sys/socket.h>
#include <uv.h>
#include <iostream>
#include <map>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>



using namespace helion;
using namespace std::string_literals;

using namespace llvm;
using namespace llvm::orc;


static LLVMContext ctx;
static IRBuilder<> builder(ctx);
static std::unique_ptr<Module> mod;
static std::unique_ptr<legacy::FunctionPassManager> fpm;
static std::unique_ptr<helion::jit> jitses;




static void init_module_and_pass_manager() {
  // Open a new module.
  mod = llvm::make_unique<Module>("my cool jit", ctx);
  mod->setDataLayout(jitses->get_target_machine().createDataLayout());
  // Create a new pass manager attached to it.
  fpm = llvm::make_unique<legacy::FunctionPassManager>(mod.get());
  // Do simple "peephole" optimizations and bit-twiddling optzns.
  fpm->add(createInstructionCombiningPass());
  // Reassociate expressions.
  fpm->add(createReassociatePass());
  // Eliminate Common SubExpressions.
  fpm->add(createGVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  fpm->add(createCFGSimplificationPass());
  fpm->doInitialization();
}



int main(int argc, char **argv) {

  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
  jitses = std::make_unique<helion::jit>();


  init_module_and_pass_manager();

  std::vector<Type *> ftype(2, Type::getDoubleTy(ctx));
  FunctionType *FT = FunctionType::get(Type::getDoubleTy(ctx), ftype, false);

  std::string name = "hello";


  llvm::Twine s = "add_doubles";
  auto *func = Function::Create(FT, Function::ExternalLinkage, 0, s, mod.get());


  std::vector<Value *> args;

  // Set names for all arguments.
  unsigned i = 0;
  for (auto &arg : func->args()) {
    std::string name = "arg";
    name += std::to_string(i++);
    arg.setName(name);
    args.push_back(&arg);
  }


  BasicBlock *BB = BasicBlock::Create(ctx, "entry", func);
  builder.SetInsertPoint(BB);

  Value *res = builder.CreateFAdd(args[0], args[1]);
  builder.CreateRet(res);

  verifyFunction(*func);

  // run the optimization on the function
  fpm->run(*func);

  // JIT the module containing the anonymous expression, keeping a handle so
  // we can free it later.
  auto H = jitses->add_module(std::move(mod));

  // Search the JIT for the __anon_expr symbol.
  auto ExprSymbol = jitses->findSymbol("add_doubles");
  assert(ExprSymbol && "Function not found");

  // Get the symbol's address and cast it to the right type (takes no
  // arguments, returns a double) so we can call it as a native function.
  auto fptr = (double (*)(double, double))(intptr_t)cantFail(ExprSymbol.getAddress());

  printf("%f\n", fptr(3, 4));

  // Delete the anonymous expression module from the JIT.
  jitses->removeModule(H);



  /*
  // print every token from the file
  text src = read_file(argv[1]);

  try {
    auto res = parse_module(src, argv[1]);
    puts(res->str());
  } catch (std::exception &e) {
    puts(e.what());
  }
  */
  return 0;
}




/**
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 */


struct storage_cell {
  size_t ind = 0;
  size_t size = 0;
  char *buf = nullptr;
};

std::vector<storage_cell> cells;



class storage_conn : public connection {
 public:
  inline void on_connect() { printf("new connection\n"); }
  inline void on_disconnect() { printf("disconnect\n"); }
  inline void on_recv(int len, const char *buf) {
    if (len > 0) {
      char cmd = buf[0];
      if (cmd == 'a') {
        size_t size = *(size_t *)(buf + 1);

        size_t ind = cells.size();

        storage_cell sc;
        sc.size = size;
        sc.ind = ind;
        sc.buf = new char[size];
        cells.push_back(sc);

        send(sizeof(size_t), (const char *)&ind);
        return;
      }


      if (cmd == 'r') {
        ssize_t *args = (ssize_t *)(buf + 1);

        auto addr = args[0];
        auto size = args[1];


        if (addr >= 0 && (size_t)addr < cells.size()) {
          auto &sc = cells[addr];

          if (sc.size >= (size_t)size) {
            send(size, (const char *)sc.buf);
            return;
          }
        }
        send("");
        return;
      }


      if (cmd == 'w') {
        ssize_t *args = (ssize_t *)(buf + 1);

        auto addr = args[0];
        auto size = args[1];

        char *data = (char *)&args[2];

        if (addr >= 0 && (size_t)addr < cells.size()) {
          auto &sc = cells[addr];

          if (sc.size >= (size_t)size) {
            memcpy(sc.buf, data, size);
            send("1");
            return;
          }
        }
        send("0");
      }
    } else {
      send("");
    }
  }
};




struct remote_storage_connection {
  int sock = -1;
  std::mutex lock;


  inline remote_storage_connection(const char *addr = "127.0.0.1",
                                   short port = 7000) {
    struct sockaddr_in serv_addr;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      printf("socket creation error \n");
      exit(-1);
    }
    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, addr, &serv_addr.sin_addr) <= 0) {
      exit(-1);
    }
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
      exit(-1);
    }
  }

  /**
   * allocate a block of memory
   */
  inline size_t alloc(size_t sz) {
    std::unique_lock lk(lock);
    size_t addr = 0;
    int blen = sizeof(char) + sizeof(size_t);
    char buf[blen];
    buf[0] = 'a';
    *(size_t *)(buf + 1) = sz;
    ::send(sock, buf, blen, 0);
    ::read(sock, &addr, sizeof(size_t));
    return addr;
  }

  inline int read(size_t addr, size_t size, char *dst) {
    std::unique_lock lk(lock);

    int blen = sizeof(char) + sizeof(size_t[2]);
    char buf[blen];
    buf[0] = 'r';
    size_t *nums = (size_t *)(buf + 1);
    nums[0] = addr;
    nums[1] = size;
    ::send(sock, buf, blen, 0);
    return ::read(sock, dst, size);
  }

  inline int write(size_t addr, int size, char *data) {
    std::unique_lock lk(lock);

    int cmdlen = sizeof(char) + sizeof(size_t) + sizeof(size_t) + size;
    char *cmd = new char[cmdlen];
    cmd[0] = 'w';
    size_t *args = (size_t *)(cmd + 1);
    args[0] = addr;
    args[1] = size;
    void *dst = &args[2];
    memcpy(dst, data, size);
    ::send(sock, cmd, cmdlen, 0);
    ::read(sock, dst, 1);
    delete[] cmd;
    return 0;
  }
};



/**
 * primary remote storage connection
 */
remote_storage_connection *rsc;

template <typename T>
class remote_ref {
 public:
  size_t ind = -1;

  operator T() {
    T v;
    rsc->read(ind, sizeof(T), (char *)&v);
    return v;
  }

  T operator+(T &o) {
    T self = *this;
    return self + o;
  }

  remote_ref &operator=(const T &value) {
    // write the changes over the network
    rsc->write(ind, sizeof(T), (char *)&value);
    return *this;
  }
};

template <typename T>
remote_ref<T> remote_alloc(void) {
  remote_ref<T> r;
  r.ind = rsc->alloc(sizeof(T));
  return r;
}


int client_main() {
  rsc = new remote_storage_connection();


  auto r = remote_alloc<int>();
  r = 0;

  while (true) {
    int i = r;
    i++;
    printf("%d\n", i);
    r = i;
  }

  return 0;
}



uv_loop_t *loop;
tcp_server<storage_conn> *server;


int _main(int argc, char **argv) {
  if (argc == 2) {
    loop = uv_default_loop();


    if (!strcmp(argv[1], "client")) {
      return client_main();
    }
    if (!strcmp(argv[1], "server")) {
      server = new tcp_server<storage_conn>(loop, "0.0.0.0", 7000);
    }


    return uv_run(loop, UV_RUN_DEFAULT);
  }
  return 0;
}
