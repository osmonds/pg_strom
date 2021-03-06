/*
 * pg_strom.h
 *
 * Header file of pg_strom module
 *
 * --
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#ifndef PG_STROM_H
#define PG_STROM_H
#include "fmgr.h"
#include "lib/ilist.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include <pthread.h>
#include <CL/cl.h>
#include "opencl_common.h"
#include "strom_types.h"

#ifndef PG_USE_INLINE
#define IF_INLINE	inline
#else
#define IF_INLINE
#endif

/*
 * mqueue.c
 */
extern pgstrom_queue *pgstrom_create_queue(void);
extern bool pgstrom_enqueue_message(pgstrom_message *message);
extern void pgstrom_reply_message(pgstrom_message *message);
extern pgstrom_message *pgstrom_dequeue_message(pgstrom_queue *queue);
extern pgstrom_message *pgstrom_try_dequeue_message(pgstrom_queue *queue);
extern pgstrom_message *pgstrom_dequeue_server_message(void);
extern void pgstrom_close_queue(pgstrom_queue *queue);
extern void pgstrom_init_message(pgstrom_message *msg,
								 MessageTag mtag,
								 pgstrom_queue *respq,
								 void (*cb_release)(pgstrom_message *msg));
extern void pgstrom_setup_mqueue(void);
extern void pgstrom_init_mqueue(void);

/*
 * gpuscan.c
 */
extern void pgstrom_init_gpuscan(void);

/*
 * opencl_devinfo.c
 */
extern pgstrom_platform_info *
collect_opencl_platform_info(cl_platform_id platform);
extern pgstrom_device_info *
collect_opencl_device_info(cl_device_id device);

extern int	pgstrom_get_device_nums(void);
extern const pgstrom_device_info *pgstrom_get_device_info(unsigned int index);
extern void pgstrom_setup_opencl_devinfo(List *dev_list);
extern void pgstrom_init_opencl_devinfo(void);

/*
 * opencl_devprog.c
 */
#define BAD_OPENCL_PROGRAM		((void *) ~0UL)
extern cl_program clserv_lookup_device_program(Datum dprog_key,
											   pgstrom_message *msg);
extern Datum pgstrom_get_devprog_key(const char *source, int32 extra_libs);
extern void pgstrom_put_devprog_key(Datum dprog_key);
extern void pgstrom_retain_devprog_key(Datum dprog_key);
extern void pgstrom_setup_opencl_devprog(void);
extern void pgstrom_init_opencl_devprog(void);

/*
 * opencl_entry.c
 */
extern void pgstrom_init_opencl_entry(void);
extern const char *opencl_strerror(cl_int errcode);

/*
 * opencl_serv.c
 */
extern cl_platform_id		opencl_platform_id;
extern cl_context			opencl_context;
extern cl_uint				opencl_num_devices;
extern cl_device_id			opencl_devices[];
extern cl_command_queue		opencl_cmdq[];

extern Datum pgstrom_opencl_device_info(PG_FUNCTION_ARGS);
extern bool pgstrom_is_opencl_server(void);
extern void pgstrom_init_opencl_server(void);

/*
 * shmem.c
 */
typedef struct shmem_context shmem_context;

extern shmem_context *TopShmemContext;

extern shmem_context *pgstrom_shmem_context_create(const char *name);
extern void pgstrom_shmem_context_reset(shmem_context *context);
extern void pgstrom_shmem_context_delete(shmem_context *context);
extern void *pgstrom_shmem_alloc(shmem_context *contetx, Size size);
extern void pgstrom_shmem_free(void *address);
extern void pgstrom_setup_shmem(Size zone_length,
								void *(*callback)(void *address,
												  Size length));
extern void pgstrom_init_shmem(void);

extern Datum pgstrom_shmem_block_info(PG_FUNCTION_ARGS);
extern Datum pgstrom_shmem_context_info(PG_FUNCTION_ARGS);

/*
 * codegen_expr.c
 */
typedef struct {
	List	   *type_defs;	/* list of devtype_info in use */
	List	   *func_defs;	/* list of devfunc_info in use */
	List	   *used_params;/* list of Const/Param in use */
	List	   *used_vars;	/* list of Var in use */
	int			incl_flags;	/* external libraries to be included */
} codegen_context;

extern devtype_info *pgstrom_devtype_lookup(Oid type_oid);
extern devfunc_info *pgstrom_devfunc_lookup(Oid func_oid);
extern char *pgstrom_codegen_expression(Node *expr, codegen_context *context);
extern char *pgstrom_codegen_declarations(codegen_context *context);
extern bool pgstrom_codegen_available_expression(Expr *expr);
extern void pgstrom_codegen_expr_init(void);

/*
 * gpuscan.c
 */
extern void pgstrom_init_gpuscan(void);

/*
 * main.c
 */
extern void _PG_init(void);
extern Datum pgstrom_shmem_alloc_func(PG_FUNCTION_ARGS);
extern Datum pgstrom_shmem_free_func(PG_FUNCTION_ARGS);

/*
 * opencl_*.h
 */
extern const char *pgstrom_opencl_common_code;
extern const char *pgstrom_opencl_gpuscan_code;

#endif	/* PG_STROM_H */
