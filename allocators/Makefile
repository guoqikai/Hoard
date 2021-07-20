CC = gcc

CC_FLAGS = -std=gnu99 -c -Wall -fmessage-length=0 -pipe -O3 -finline-limit=65000 -fkeep-inline-functions -finline-functions -ffast-math -fomit-frame-pointer -DNDEBUG -I. -I$(TOPDIR)/include -D_REENTRANT=1

CC_DBG_FLAGS = -c -Wall -fmessage-length=0 -pipe -g -I. -I$(TOPDIR)/include -D_REENTRANT=1

all: libkheap libmmlibc libhoard-alloc

debug: libkheap_dbg libmmlibc_dbg libhoard-alloc_dbg

alloclibs:
	mkdir alloclibs

# Library containing mm_malloc and mm_free for OS/161 kheap-based allocator

libkheap: alloclibs
	cd kheap; $(CC) $(CC_FLAGS) kheap.c; ar rs ../alloclibs/libkheap.a kheap.o

libkheap_dbg: alloclibs
	cd kheap; $(CC) $(CC_DBG_FLAGS) kheap.c; ar rs ../alloclibs/libkheap_dbg.a kheap.o 

# Library containing mm_malloc and mm_free for hoard

libhoard-alloc: alloclibs
	cd hoard-alloc; $(CC) $(CC_FLAGS) hoard-alloc.c; ar rs ../alloclibs/libhoard-alloc.a hoard-alloc.o

libhoard-alloc_dbg: alloclibs
	cd hoard-alloc; $(CC) $(CC_DBG_FLAGS) hoard-alloc.c; ar rs ../alloclibs/libhoard-alloc_dbg.a hoard-alloc.o 


# Library containing mm_malloc and mm_free wrappers for libc allocator
libmmlibc: alloclibs
	cd libc; $(CC) $(CC_FLAGS) libc_wrapper.c; ar rs ../alloclibs/libmmlibc.a libc_wrapper.o

libmmlibc_dbg: alloclibs
	cd libc; $(CC) $(CC_DBG_FLAGS) libc_wrapper.c; ar rs ../alloclibs/libmmlibc_dbg.a libc_wrapper.o

clean:
	rm -rf alloclibs; rm -f */*.o; rm -f *~; rm -f */*~
