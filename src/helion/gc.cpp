#include <stdio.h>

// Include file for GC-aware allocators.
// alloc allocates uncollected objects that are scanned by the collector
// for pointers to collectable objects.  Gc_alloc allocates objects that
// are both collectable and traced.  Single_client_alloc and
// single_client_gc_alloc do the same, but are not thread-safe even
// if the collector is compiled to be thread-safe.  They also try to
// do more of the allocation in-line.
#include <gc/gc_allocator.h>
#include <helion/gc.h>
#include <pthread.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <algorithm>
#include <mutex>

#define GC_THREADS
#include <gc/gc.h>

extern "C" void GC_allow_register_threads();

#ifdef USE_GC
#define allocate GC_MALLOC
#define deallocate GC_FREE

struct gc_startup {
  gc_startup() {
    GC_set_all_interior_pointers(1);
    GC_INIT();
    GC_allow_register_threads();
  }
};
static gc_startup init;

#else
#define allocate helion::gc::malloc
#define deallocate helion::gc::free
#endif

#define ROUND_UP(N, S) ((((N) + (S)-1) / (S)) * (S))

void* operator new(size_t size) { return allocate(size); }
void* operator new[](size_t size) { return allocate(size); }

#ifdef __GLIBC__
#define _NOEXCEPT _GLIBCXX_USE_NOEXCEPT
#endif

void operator delete(void* ptr)_NOEXCEPT { deallocate(ptr); }

void operator delete[](void* ptr) _NOEXCEPT { deallocate(ptr); }

void operator delete(void* ptr, std::size_t s)_NOEXCEPT { deallocate(ptr); }

void operator delete[](void* ptr, std::size_t s) _NOEXCEPT { deallocate(ptr); }




/* single word (4) or double word (8) alignment */
#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7)
#define PAGE_SIZE_ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7)
/* rounds up to the nearest multiple of ALIGNMENT */
#define HEADER_SIZE (ALIGN(sizeof(gc::blk_t)))

#define OVERHEAD (ALIGN(sizeof(gc::heap_segment::free_header)))

// define some linked list operations
#define ll_fwd(name) ((name) = (name)->next)
#define GET_FREE_HEADER(blk) \
  ((gc::heap_segment::free_header*)((char*)blk + HEADER_SIZE))
#define ADJ_SIZE(given) ((given < OVERHEAD) ? OVERHEAD : ALIGN(given))
#define GET_BLK(header) ((blk_t*)((char*)(header)-HEADER_SIZE))
#define IS_FREE(blk) (*(blk)&1UL)
#define GET_SIZE(blk) (*(blk) & ~1UL)
#define SET_FREE(blk) (*(blk) |= 1UL)
#define SET_USED(blk) (*(blk) &= ~1UL)
#define SET_SIZE(blk, newsize) (*(blk) = ((newsize) & ~1UL) | IS_FREE(blk))
#define NEXT_BLK(blk) (blk_t*)((char*)(blk) + GET_SIZE(blk))


/**
 * an attempt to make my own GC:
 */
using namespace helion;
using namespace helion::gc;





static void add_heap(gc::heap_segment *hs) {
}



// define the number of pages in a block
// TODO(optim) decide on how many pages a block should have
//             in it.
int gc::heap_segment::page_count = 1;

/**
 * allocate a new block with a minimum number of bytes in the heap. It will
 * always allocate 1 extra page for administration overhead.
 */
heap_segment* gc::heap_segment::alloc(size_t size) {
  heap_segment* b = nullptr;
  size = PAGE_SIZE_ALIGN(size);
  printf("allocating block of size %zx\n", size);

  void* mapped_region = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  printf("%p %zu\n", mapped_region, size);

  b = (heap_segment*)mapped_region;
  b->size = size;


  size_t real_size = size - ALIGN(sizeof(heap_segment));
  printf("++ %zx\n", real_size);
  // the first thing that needs to be setup in a block segment is
  // the initial header.
  blk_t* hdr = (blk_t*)(b + 1);
  b->first_block = hdr;
  SET_FREE(hdr);
  SET_SIZE(hdr, size - ALIGN(sizeof(heap_segment)));

  free_header* fh = GET_FREE_HEADER(hdr);
  fh->next = &b->free_entry;
  fh->prev = &b->free_entry;
  b->free_entry.next = fh;
  b->free_entry.prev = fh;
  return b;
}

#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_RESET "\x1b[0m"

void gc::heap_segment::dump(void) {
  blk_t* top = (blk_t*)((char*)this + size);
  blk_t* c = first_block;
  for (; c < top; c = NEXT_BLK(c)) {
    auto color = IS_FREE(c) ? ANSI_COLOR_GREEN : ANSI_COLOR_RED;
    printf("%s%li%s ", color, GET_SIZE(c), ANSI_COLOR_RESET);
    printf(ANSI_COLOR_RESET);
  }
  printf("\n\n");
}



void* gc::heap_segment::malloc(size_t size) {
  size = ADJ_SIZE(size);

  free_header* fit = find_fit(size);
  if (fit == nullptr) {
    return nullptr;
  }

  // at this point, we need to split the block, and insert the
  blk_t* blk = GET_BLK(fit);
  blk_t* split_block = split(blk, size);

  // if the split block is the same block there wasn't a split
  // because I place the new block at the end of the old big block
  if (split_block == blk) {
    printf("OPES: %p %p\n", fit->prev, fit->next);
    fit->prev->next = fit->next;
    fit->next->prev = fit->prev;
  }
  blk = split_block;

  SET_USED(blk);
  dump();
  return (char*)blk + HEADER_SIZE;
}


/**
 * given a block, split it into two blocks. If the change in block size
 * would result in an invalid free_header on one, simply return the original
 * block
 */
blk_t* gc::heap_segment::split(blk_t* blk, size_t size) {
  size_t current_size = GET_SIZE(blk);
  size_t target_block_size = size + HEADER_SIZE;

  size_t final_size = current_size - target_block_size;

  if (final_size < (size_t)OVERHEAD) return blk;

  blk_t* split_block = (blk_t*)((char*)blk + final_size);
  // only change the block's size if it is a different memory address
  if (blk != split_block) {
    SET_SIZE(blk, final_size);
    SET_SIZE(split_block, target_block_size);
  }
  return split_block;
}



gc::heap_segment::free_header* gc::heap_segment::find_fit(size_t size) {
  free_header* h = &free_entry;
  h = h->next;

  while (h != &free_entry && h != nullptr) {
    blk_t* b = GET_BLK(h);
    if (GET_SIZE(b) >= size) {
      return h;
    }
    h = h->next;
  }
  return nullptr;
}

void* gc::heap_segment::mem_heap_lo(void) { return first_block; }
void* gc::heap_segment::mem_heap_hi(void) {
  return (void*)((char*)this + size);
}




// TODO(threads) have seperate regions and whatnot
thread_local void** stack_root;
static thread_local heap_segment* heap;

void gc::set_stack_root(void* sb) {
  //
  stack_root = (void**)sb;
}

void* gc::malloc(size_t s) {
  /*
   * allocation requires a heap on the current thread.
   */
  if (heap == nullptr) {
    heap = gc::heap_segment::alloc();
  }
top:
  /*
   * attempt to allocate the memory in the current heap segment.
   */
  void* p = heap->malloc(s);
  if (p == nullptr) {
    // if the pointer was null, we can assume that there was no
    // space to allocate that memory in the current heap. So it
    // is up to the current allocation call to move the current
    // heap to the heap tree. Then we either allocate a new heap
    // in this function and re-attempt the allocation
    printf("OLD HEAP, PUT IN SOME KIND OF TREE: %p\n", heap);
    s = std::max(s, (size_t)gc::heap_segment::page_count * 4096);
    exit(-1);
    heap = gc::heap_segment::alloc();
    goto top;
  }

  return p;
}




void gc::free(void* ptr) { printf("NEED TO FREE\n"); }

void gc::collect(void) {}
