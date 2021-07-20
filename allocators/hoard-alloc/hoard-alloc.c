#define _GNU_SOURCE
#include <sched.h>
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <strings.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include "memlib.h"


#define SUPER_BLOCK_SIZE  4096
#define PAGE_SIZE 4096
#define EMPTY_FRACTION 0.25
#define K_FREE 8
#define PAGE_FRAME 0xfffffffffffff000 //I assume the platfrom is 64 bit
#define BIN_SIZE 4
#define CACHE_LINE_SIZE 64

/* we have base=8 and b=2 here */
#define NSIZES 9
static const size_t sizes[NSIZES] = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define SMALLEST_BLOCK_SIZE 8
#define LARGEST_BLOCK_SIZE 2048

//we want to make these two sizes as large as possible to avoid collision
#define PROCESSOR_HASHTABLE_SIZE 64

#define SBR_PAGEADDR(sbr)  ((sbr)->pageaddr_and_blocktype & PAGE_FRAME)
#define SBR_BLOCKTYPE(sbr) ((sbr)->pageaddr_and_blocktype & ~PAGE_FRAME)
#define MKPAB(pa, blk)   (((pa)&PAGE_FRAME) | ((blk) & ~PAGE_FRAME))

#define BIG_CHUNK 0xbeadccbe // mask for big chunk memory

#define IS_BIG_CHUNK(addr) (((addr) & 0xffffffff00000000) == 0xbeadccbe00000000)




typedef ptrdiff_t vaddr_t;

static void fill_deadbeef(void *vptr, size_t len) {
    u_int32_t *ptr = vptr;
    size_t i;
    for (i=0; i<len/sizeof(u_int32_t); i++) {
        ptr[i] = 0xdeadbeef;
    }
}

typedef struct freelist {
    struct freelist *next;
} freelist_t;

typedef struct big_freelist {
    int npages;
    struct big_freelist *next;
} big_freelist_t;

struct superblock_ref;

typedef struct processor_heap {
    int cpu_id;
    int superblock_count;
    size_t usage;
    pthread_mutex_t ph_lock;
    struct superblock_ref *sizebases_bin[NSIZES][BIN_SIZE]; //2d array, where first entry is type, second is bin
    struct processor_heap *next;
    unsigned char padding[32];
} processor_heap_t;

typedef struct superblock_ref {
    struct superblock_ref *next;
    struct superblock_ref *prev;
    freelist_t *flist;
    vaddr_t pageaddr_and_blocktype;
    pthread_mutex_t ref_lock;
    size_t usage;
    processor_heap_t *owner;
    unsigned char padding[40];
} superblock_ref_t;

_Static_assert(sizeof(superblock_ref_t) % CACHE_LINE_SIZE == 0, "align it to cache line size!");
_Static_assert(sizeof(processor_heap_t) % CACHE_LINE_SIZE == 0, "align it to cache line size!");

static processor_heap_t *processor_heaps_lookup_table[PROCESSOR_HASHTABLE_SIZE];
static processor_heap_t *fresh_processor_heap;
static superblock_ref_t *fresh_refs;
static big_freelist_t *big_chunks;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

/* the last bin is for completely full block
 */
static inline unsigned get_bin_index(size_t usage) {
    return  usage == SUPER_BLOCK_SIZE ? BIN_SIZE - 1 : (usage * (BIN_SIZE - 1)) / SUPER_BLOCK_SIZE;
}


static processor_heap_t *get_processor_heap(int cpu_id) {
    // since 0 is for global heap
    processor_heap_t *ph = processor_heaps_lookup_table[(cpu_id % (PROCESSOR_HASHTABLE_SIZE - 1)) + 1];
    
    while (ph && ph->cpu_id != cpu_id) {
        ph = ph->next;
    }
    return ph;
}


static processor_heap_t *alloc_processor_heap(void) {
    processor_heap_t *ph;
    
    pthread_mutex_lock(&global_lock);
    
    if (fresh_processor_heap) {
        ph = fresh_processor_heap;
        fresh_processor_heap = fresh_processor_heap->next;
        ph->next = NULL;
        pthread_mutex_unlock(&global_lock);
        return ph;
    }
    
    ph = (processor_heap_t *)mem_sbrk(SUPER_BLOCK_SIZE);
    
    if (ph) {
        bzero(ph, SUPER_BLOCK_SIZE);
        fresh_processor_heap = ph+1;
        processor_heap_t *tmp = ph;
        int nrefs = SUPER_BLOCK_SIZE / sizeof(processor_heap_t) - 1;
        int i;
        for (i = 0; i < nrefs-1; i++) {
            pthread_mutex_init(&(tmp->ph_lock), NULL);
            tmp->next = tmp+1;
            tmp = tmp->next;
        }
        tmp->next = NULL;
        ph->next = NULL;
    }
    ph->next = NULL;
    pthread_mutex_unlock(&global_lock);
    return ph;
    
}

/*
Lock targe_ph before call this! Allocate a superblock from memory, initialized the internal structure of the
 superblock and return the correspond super block reference.
 */
static superblock_ref_t *alloc_superblock_mem(unsigned size_type, processor_heap_t *target_ph) {
    superblock_ref_t *ref;
    int i;
    pthread_mutex_lock(&global_lock);
    
    if (fresh_refs) {
        ref = fresh_refs;
        fresh_refs = fresh_refs->next;
    }
    else {
        ref = (superblock_ref_t *)mem_sbrk(SUPER_BLOCK_SIZE);
        if (!ref) {
            pthread_mutex_unlock(&global_lock);
            return NULL;
        }
        bzero(ref, SUPER_BLOCK_SIZE);
        fresh_refs = ref+1;
        superblock_ref_t *tmp = fresh_refs;
        int nrefs = SUPER_BLOCK_SIZE / sizeof(superblock_ref_t) - 1;
        int i;
        for (i = 0; i < nrefs-1; i++) {
            pthread_mutex_init(&(tmp->ref_lock), NULL);
            tmp->next = tmp+1;
            tmp = tmp->next;
        }
        tmp->next = NULL;
    }
    

    vaddr_t sbrpage = (vaddr_t)mem_sbrk(SUPER_BLOCK_SIZE);
    if (!sbrpage) {
        pthread_mutex_unlock(&global_lock);
        return NULL;
    }
    
    ref->pageaddr_and_blocktype = MKPAB(sbrpage, size_type);
    int num_free = (SUPER_BLOCK_SIZE / sizes[size_type]);
    vaddr_t fla = sbrpage;
    //store pointer to superblock ref in the first sub block
    *((superblock_ref_t**)fla) = ref;

    freelist_t *fl = (freelist_t *)(fla + sizes[size_type]);
    fl->next = NULL;
    for (i = 2; i < num_free; i++) {
        fl = (freelist_t *)(fla + i * sizes[size_type]);
        fl->next = (freelist_t *)(fla + (i - 1) * sizes[size_type]);
        assert(fl != fl->next);
    }
    ref->flist = fl;
    ref->usage = sizes[size_type];
    assert((vaddr_t)ref->flist == sbrpage + (num_free - 1) * sizes[size_type]);
    ref->owner = target_ph;
    if (target_ph->sizebases_bin[size_type][0]) {
        target_ph->sizebases_bin[size_type][0]->prev = ref;
    }
    ref->prev = NULL;
    ref->next = target_ph->sizebases_bin[size_type][0];
    target_ph->sizebases_bin[size_type][0] = ref;
    target_ph->superblock_count += 1;
    pthread_mutex_unlock(&global_lock);
    return ref;
}


// kheap rip off
static inline int superblock_type(size_t sz) {
    unsigned i;
    for (i=0; i<NSIZES; i++) {
        if (sz <= sizes[i]) {
            return i;
        }
    }
    
    printf("Subpage allocator cannot handle allocation of size %lu\n",
           (unsigned long)sz);
    exit(1);
    // keep compiler happy
    return 0;
}

static void *big_kmalloc(size_t sz) {
    
    void *result = NULL;
    
    sz += SMALLEST_BLOCK_SIZE;
    /* Round up to a whole number of pages. */
    int npages = (sz + SUPER_BLOCK_SIZE - 1)/SUPER_BLOCK_SIZE;
    
    pthread_mutex_lock(&global_lock);
    
    /* Check if we happen to have a chunk of the right size already */
    struct big_freelist *tmp = big_chunks;
    struct big_freelist *prev = NULL;
    while (tmp != NULL) {
        if (tmp->npages > npages) {
            /* Carve the block in two pieces */
            tmp->npages -= npages;
            int *hdr_ptr = (int *)((char *)tmp+(tmp->npages*SUPER_BLOCK_SIZE));
            *hdr_ptr = npages;
            *(hdr_ptr + 1) = BIG_CHUNK;
            result = (void *)((char *)hdr_ptr + SMALLEST_BLOCK_SIZE);
            break;
        } else if (tmp->npages == npages) {
            /* Remove block from freelist */
            if (prev) {
                prev->next = tmp->next;
            } else {
                big_chunks = tmp->next;
            }
            int *hdr_ptr = (int *)tmp;
            assert(*hdr_ptr == npages);
            result = (void *)((char *)hdr_ptr + SMALLEST_BLOCK_SIZE);
            break;
        } else {
            prev = tmp;
            tmp = tmp->next;
        }
    }
    
    if (result == NULL) {
        /* Nothing suitable in freelist... grab space with mem_sbrk */
        int *hdr_ptr = (int *)mem_sbrk(npages * SUPER_BLOCK_SIZE);
        if (hdr_ptr != NULL) {
            *hdr_ptr = npages;
            *(hdr_ptr + 1) = BIG_CHUNK;
            result = (void *)((char *)hdr_ptr + SMALLEST_BLOCK_SIZE);
        }
    }
    
    pthread_mutex_unlock(&global_lock);
    
    return result;
}

static void big_kfree(void *ptr) {
    /* Coalescing is unlikely to do much good (other page allocations
     * for small objects are likely to prevent big chunks from fitting
     * together), so we don't bother trying.
     */
    
    int *hdr_ptr = (int *)((char *)ptr - SMALLEST_BLOCK_SIZE);
    //int npages = *hdr_ptr;
    
    pthread_mutex_lock(&global_lock);
    
    struct big_freelist *newfree = (struct big_freelist *) hdr_ptr;
    assert(newfree->npages == *hdr_ptr);
    newfree->next = big_chunks;
    big_chunks = newfree;
    
    pthread_mutex_unlock(&global_lock);
}
// end of kheap rip off


/* lock ph before call this function */
static void move_bin(unsigned from, unsigned to, processor_heap_t *ph, superblock_ref_t *ref) {
    unsigned size_type = SBR_BLOCKTYPE(ref);
    if (ref->next) {
        ref->next->prev = ref->prev;
    }
    if (ref->prev) {
        ref->prev->next = ref->next;
    }
    else {
        ph->sizebases_bin[size_type][from] = ref->next;
    }
    ref->prev = NULL;
    if (ph->sizebases_bin[size_type][to]) {
        ph->sizebases_bin[size_type][to]->prev = ref;
    }
    ref->next = ph->sizebases_bin[size_type][to];
    ph->sizebases_bin[size_type][to] = ref;
}

/* lock ph before call this function */
static superblock_ref_t *find_super_block_ph(unsigned size_type, processor_heap_t *ph) {
    superblock_ref_t *ref;
    
    int i;
    // from fullest bin, the last bin is for fully occupied super blocks so we don't check
    for (i = BIN_SIZE - 2; i >= 0; i --) {
        ref = ph->sizebases_bin[size_type][i];
        if (ref) {
            break;
        }
    }
    return ref;
}

/* lock ph before call this function */
static superblock_ref_t *pop_ref_from_global_heap(unsigned size_type, processor_heap_t *target_ph) {

    processor_heap_t *gph = processor_heaps_lookup_table[0];
    pthread_mutex_lock(&(gph->ph_lock));

    superblock_ref_t *ref = find_super_block_ph(size_type, gph);
    if (ref) {
        unsigned bin_ind = get_bin_index(ref->usage); 
        pthread_mutex_lock(&(ref->ref_lock));
        if (ref->next) {
            ref->next->prev = ref->prev;
        }
        if (ref->prev) {
            ref->prev->next = ref->next;
        }
        else {
            gph->sizebases_bin[size_type][bin_ind] = ref->next;
        }
        ref->prev = NULL;
        gph->superblock_count -= 1;
        gph->usage -= ref->usage;
        ref->owner = target_ph;
        target_ph->usage += ref->usage;
        target_ph->superblock_count += 1;
        if (target_ph->sizebases_bin[size_type][bin_ind]) {
            target_ph->sizebases_bin[size_type][bin_ind]->prev = ref;
        }
        ref->next = target_ph->sizebases_bin[size_type][bin_ind];
        target_ph->sizebases_bin[size_type][bin_ind] = ref;
        pthread_mutex_unlock(&(ref->ref_lock));
    }
    pthread_mutex_unlock(&(gph->ph_lock));
    return ref;
}

static void *block_malloc(size_t sz) {
    
    unsigned size_type;    // index into sizes[] that we're using
    superblock_ref_t *ref;    // super_block_ref for superblock we're allocating from
    void *retptr;        // our result
    
    
    size_type = superblock_type(sz);
    sz = sizes[size_type];
    
    int cpu_id = sched_getcpu();
    processor_heap_t *ph = get_processor_heap(cpu_id);
    if (!ph) {
        ph = alloc_processor_heap();
        if (!ph) {
            printf("malloc: couldn't get processor heap!\n");
            exit(1);
        }
        ph->cpu_id = cpu_id;
        int hash_index = (cpu_id % (PROCESSOR_HASHTABLE_SIZE - 1)) + 1;
        processor_heap_t *head = processor_heaps_lookup_table[hash_index];
        if (!head) {
            processor_heaps_lookup_table[hash_index] = ph;
        }
        else {
            head->next = ph;
        }
    }
    pthread_mutex_lock(&(ph->ph_lock));
    ref = find_super_block_ph(size_type, ph);
    // use switch case might look more elegant?
    if (!ref) {
        ref = pop_ref_from_global_heap(size_type, ph);
        if (!ref) {
             ref = alloc_superblock_mem(size_type, ph);
            if (!ref) {
                pthread_mutex_unlock(&(ph->ph_lock));
                printf("malloc: couldn't alloc memory!\n");
                exit(1);
            }
        }

    }
    //ok now lock the ref
    pthread_mutex_lock(&(ref->ref_lock));
    retptr = ref->flist;
    ref->flist = ref->flist->next;
    if (get_bin_index(ref->usage) != get_bin_index(ref->usage + sz)) {
        move_bin(get_bin_index(ref->usage), get_bin_index(ref->usage + sz), ph, ref);
    }
    ref->usage += sz;
    ph->usage += sz;
    pthread_mutex_unlock(&(ref->ref_lock));
    pthread_mutex_unlock(&(ph->ph_lock));
    return retptr;
}

/* lock ph before call this */
static void push_superblock_to_global(processor_heap_t *ph) {
    
    superblock_ref_t *target = NULL;
    superblock_ref_t *tmp;
    size_t min_usage = SUPER_BLOCK_SIZE;
    unsigned size_type;
    unsigned min_size_type = 0;
    int i;
    
    //find this block with least usage
    for (i = 0; i < BIN_SIZE - 1; i++) {
        for (size_type = 0; size_type < NSIZES; size_type ++) {
            if (ph->sizebases_bin[size_type][i]) {
                tmp = ph->sizebases_bin[size_type][i];
                while (tmp) {
                    if (tmp->usage <=  min_usage) {
                        min_usage = tmp->usage;
                        target = tmp;
                        min_size_type = size_type;
                    }
                    tmp = tmp->next;
                }
            }
        }
        if (target) {
            if (target->next) {
                target->next->prev = target->prev;
            }
            if (target->prev) {
                target->prev->next = target->next;
            }
            else {
                ph->sizebases_bin[min_size_type][i] = target->next;
            }
            break;
        }
        
    }
    
    assert(!target);
    ph->superblock_count -= 1;
    ph->usage -= target->usage;
    
    processor_heap_t *gph = processor_heaps_lookup_table[0];
    pthread_mutex_lock(&(gph->ph_lock));
    pthread_mutex_lock(&(target->ref_lock));
    target->owner = gph;
    target->prev = NULL;
    if (gph->sizebases_bin[min_size_type][i]) {
        gph->sizebases_bin[min_size_type][i]->prev = target;
    }
    target->next = gph->sizebases_bin[min_size_type][i];
    gph->sizebases_bin[min_size_type][i] = target;
    gph->superblock_count +=1;
    gph->usage += target->usage;
    
    pthread_mutex_unlock(&(target->ref_lock));
    pthread_mutex_unlock(&(gph->ph_lock));
}


static void block_free(void *ptr, superblock_ref_t *ref) {
    processor_heap_t *ph; //owner
    unsigned size_type;
    vaddr_t ptraddr;    // same as ptr
    vaddr_t sbrpage;      // SBR_PAGEADDR(ref)
    vaddr_t offset;       // offset into page
    ptraddr = (vaddr_t)ptr;
    while (1) {
        pthread_mutex_lock(&(ref->ref_lock));
        ph = ref->owner;
        if (pthread_mutex_trylock(&((ref->owner)->ph_lock)) == 0) {
            break;
        }
        else {
            pthread_mutex_unlock(&(ref->ref_lock));
            pthread_mutex_lock(&(ph->ph_lock)); //possible starving, hope the scheduler is fair
            pthread_mutex_lock(&(ref->ref_lock));
            if (ref->owner == ph) {
                break;
            }
            // ok the ownership transferred to some other heap, do the whole stuff one more time
            pthread_mutex_unlock(&(ref->ref_lock));
            pthread_mutex_unlock(&(ph->ph_lock));
        }
    }
    // after exist the loop, we should have acquired both ref's lock and ref's owner's lock
    sbrpage = SBR_PAGEADDR(ref);
    size_type = SBR_BLOCKTYPE(ref);
    offset = ptraddr - sbrpage;
    if (offset == 0 || offset >= SUPER_BLOCK_SIZE || offset % sizes[size_type] != 0) {
        printf("free:free of invalid addr %p\n", ptr);
        exit(1);
    }
    
    /*
     * Clear the block to 0xdeadbeef to make it easier to detect
     * uses of dangling pointers.
     */
    fill_deadbeef(ptr, sizes[size_type]);
    ((struct freelist *)ptr)->next = ref->flist;
    ref->flist = (struct freelist *)ptr;
    move_bin(get_bin_index(ref->usage), get_bin_index(ref->usage - sizes[size_type]), ph, ref);
    
    ref->usage -= sizes[size_type];
    ph->usage -= sizes[size_type];
    pthread_mutex_unlock(&(ref->ref_lock));
    if (ph == processor_heaps_lookup_table[0] || ph->superblock_count == 0) {
        pthread_mutex_unlock(&(ph->ph_lock));
        return;
    }
    if ((ph->usage < EMPTY_FRACTION * ph->superblock_count * SUPER_BLOCK_SIZE) && 
        ((ph->superblock_count * SUPER_BLOCK_SIZE - ph->usage)/SUPER_BLOCK_SIZE > K_FREE)) {
        push_superblock_to_global(ph);
    }
    
    pthread_mutex_unlock(&(ph->ph_lock));
}



////////////////////////////////////////////////////////////

void *mm_malloc(size_t sz) {
    if (sz > SUPER_BLOCK_SIZE/2) {
        return big_kmalloc(sz);
    }
    return block_malloc(sz);
}

void mm_free(void *ptr) {
    vaddr_t ptraddr = (vaddr_t)ptr;
    vaddr_t loaddr = (vaddr_t)dseg_lo;
    vaddr_t *ref = (vaddr_t*)(((ptraddr - loaddr) / SUPER_BLOCK_SIZE) * SUPER_BLOCK_SIZE + loaddr);
    if (IS_BIG_CHUNK((vaddr_t)(*ref))) {
        big_kfree(ptr);
        return;
    }
    block_free(ptr, (superblock_ref_t*)(*ref));
    return;
}


int mm_init(void) {   
    if (dseg_lo == NULL && dseg_hi == NULL) {
        if (mem_init() != 0) {
            return -1;
        }
    }
    processor_heaps_lookup_table[0] = alloc_processor_heap();
    return 0;
}

