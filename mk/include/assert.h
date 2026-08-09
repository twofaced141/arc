#ifndef ASSERT_H
#define ASSERT_H

#ifdef CONFIG_DEBUG

extern void panic_simple(const char *reason);

#define __ASSERT_STR2(x) #x
#define __ASSERT_STR(x)  __ASSERT_STR2(x)

#define ASSERT(cond) \
    do { \
        if (!(cond)) \
            panic_simple("ASSERT(" #cond ") at " __FILE__ ":" __ASSERT_STR(__LINE__)); \
    } while (0)

#define VERIFY(cond) \
    ({ \
        int _v_ = !!(cond); \
        if (!_v_) \
            panic_simple("VERIFY(" #cond ") at " __FILE__ ":" __ASSERT_STR(__LINE__)); \
        _v_; \
    })

#define VM_VERIFY(cond)    ASSERT(cond)
#define LOCK_VERIFY(cond)  ASSERT(cond)
#define VFS_VERIFY(cond)   ASSERT(cond)

#else

#define ASSERT(cond)         ((void)0)
#define VERIFY(cond)         (!!(cond))
#define VM_VERIFY(cond)      ((void)0)
#define LOCK_VERIFY(cond)    ((void)0)
#define VFS_VERIFY(cond)     ((void)0)

#endif

#endif
