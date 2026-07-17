/** @file No-op project logging macros for integration host tests. */
#ifndef __CROSS_LAYER_MT_LOG_H__
#define __CROSS_LAYER_MT_LOG_H__

#define DBG_ERROR 1
#define DBG_WARN 2
#define DBG_INFO 3
#define DBG_DEBUG 4
#define DBG_VERBOSE 5

#define LOG_E(...) ((void)0)
#define LOG_W(...) ((void)0)
#define LOG_I(...) ((void)0)
#define LOG_D(...) ((void)0)
#define LOG_V(...) ((void)0)

#endif /* __CROSS_LAYER_MT_LOG_H__ */
