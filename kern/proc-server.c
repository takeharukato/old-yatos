/* -*- mode: C; coding:utf-8 -*- */
/**********************************************************************/
/*  Yet Another Teachable Operating System                            */
/*  Copyright 2016 Takeharu KATO                                      */
/*                                                                    */
/*  process service routines                                          */
/*                                                                    */
/**********************************************************************/

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kern/config.h>
#include <kern/kernel.h>
#include <kern/param.h>
#include <kern/kern_types.h>
#include <kern/assert.h>
#include <kern/kprintf.h>
#include <kern/string.h>
#include <kern/errno.h>
#include <kern/spinlock.h>
#include <kern/proc.h>
#include <kern/thread.h>
#include <kern/vm.h>
#include <kern/lpc.h>
#include <kern/kname-service.h>
#include <kern/proc-service.h>
#include <kern/async-event.h>
#include <kern/page.h>

static thread *proc_service_thr;

//#define DEBUG_PROC_SERVICE
//#define DEBUG_PROC_SERVICE_

#define MSG_PREFIX "PROC:"

/** send_eventシステムコールの実処理
    @param[in] sndev   send_eventメッセージ
    @param[in] src     呼出元エンドポイント
    @retval    0       正常に送信した
    @retval   -ENOENT  要求元が存在しない
    @retval   -ENOMEM  メモリ不足により送信に失敗した
 */
static int
handle_send_event(proc_sys_send_event *sndev, endpoint src) {
	int           rc;
	thread      *thr;
	event_node *node;
	evinfo     *info;

	thr = thr_find_thread_by_tid(src);
	if ( thr == NULL ) 
		return -ENOENT;
	rc = ev_alloc_node(sndev->id, &node);	
	if ( rc != 0 ) 
		return rc;
	info = &node->info;

	info->sender_id = thr->tid;
	info->code = EV_SIG_SI_USER;
	info->data = sndev->data;

	rc = ev_send(sndev->dest, node);
	if ( rc != 0 )
		goto free_mem_out;

	return 0;

free_mem_out:
	kfree( node );
	return rc;
}

/** create_threadシステムコールの実処理
    @param[in] crethr  create_threadメッセージ
    @param[in] src     呼出元エンドポイント
    @retval    0       正常に送信した
    @retval   -ENOENT  要求元が存在しない
    @retval   -ENOMEM  メモリ不足により生成に失敗した
    @retval   -EINVAL  ユーザスレッドに指定不可能な優先度を指定した
    @retval   -EFAULT  ユーザからアクセスできないスタックや開始アドレスを指定した
 */
static int
handle_create_thread(proc_sys_create_thread *crethr, endpoint src) {
	int               rc;
	thread         *pthr;
	thread      *new_thr;

	kassert( crethr != NULL );

	pthr = thr_find_thread_by_tid(src);
	if ( pthr == NULL ) 
		return -ENOENT;

	kassert( pthr->p != hal_refer_kernel_proc() );

	if ( crethr->prio >= THR_MAX_USER_PRIO )
		return -EINVAL;

	if ( !vm_user_area_can_access(&pthr->p->vm, crethr->sp, 
		PAGE_SIZE, VMA_PROT_R|VMA_PROT_W) )
		return -EFAULT;

	if ( !vm_user_area_can_access(&pthr->p->vm, crethr->start, 
		PAGE_SIZE, VMA_PROT_X) )
		return -EFAULT;
	
	rc = thr_new_thread(&new_thr);  /*  スレッドを作成 */
	if ( rc != 0 )
		goto error_out;

	rc = thr_create_uthread(new_thr, crethr->prio, THR_FLAG_JOINABLE, pthr->p, 
	    crethr->start, (uintptr_t)crethr->arg1, (uintptr_t)crethr->arg2, (uintptr_t)crethr->arg3, 
	    (void *)crethr->sp);
	if ( rc != 0 )
		goto free_thread_out;

	crethr->id = new_thr->tid;

	rc = thr_start(new_thr, pthr->tid);
	if ( rc != 0 )
		goto free_thread_out;

	return 0;

free_thread_out:
	thr_destroy( new_thr );
error_out:
	return rc;
}

/** getrusageシステムコールの実処理
    @param[in] getrusg getrusageメッセージ
    @param[in] src     呼出元エンドポイント(未使用)
    @retval    0       正常に獲得した
    @retval   -ENOENT  対象のスレッドがが存在しない
 */
static int
handle_get_thread_resource(proc_sys_getrusage *getrusg, endpoint __attribute__ ((unused)) src) {
	thread          *thr;
	intrflags      flags;

	kassert( getrusg != NULL );

	thr = thr_find_thread_by_tid(getrusg->id);
	if ( thr == NULL ) 
		return -ENOENT;

	spinlock_lock_disable_intr( &thr->lock, &flags );
	memcpy( &getrusg->res, &thr->resource, sizeof(thread_resource) );
	spinlock_unlock_restore_intr( &thr->lock, &flags );

	return 0;
}

/** プロセスサービス処理部
    @param[in] arg スレッド引数(未使用)
 */
static int
handle_proc_service(void __attribute__ ((unused)) *arg) {
	msg_body                   msg;
	proc_service             *smsg;
	proc_sys_send_event     *sndev;
	proc_sys_create_thread *crethr;
	proc_sys_getrusage    *getrusg;
	endpoint                   src;
	int                         rc;

	rc = kns_register_kernel_service(ID_RESV_NAME_PROC);
	kassert( rc == 0 );
	
	while(1) {

		memset( &msg, 0, sizeof(msg_body) );
		smsg  = &msg.proc_msg;

		rc = lpc_recv(LPC_RECV_ANY, LPC_INFINITE, &msg, &src);
		kassert( rc == 0 );

#if defined(DEBUG_PROC_SERVICE)
		kprintf(KERN_INF, MSG_PREFIX "tid=%d thread=%p (src, req)=(%d, %d)\n", 
		    current->tid, current, src, smsg->req);
#endif  /*  DEBUG_PROC_SERVICE  */

		switch( smsg->req ) {
		case PROC_SERV_REQ_SNDEV:

			sndev = &smsg->proc_service_calls.sndev;
			smsg->rc = handle_send_event(sndev, src);
			
			rc = lpc_send(src, LPC_INFINITE, &msg);
			kassert( rc == 0 );
			break;
		case PROC_SERV_REQ_CRETHR:

			crethr = &smsg->proc_service_calls.crethr;
			smsg->rc = handle_create_thread(crethr, src);
			rc = lpc_send(src, LPC_INFINITE, &msg);
			kassert( rc == 0 );
			break;
		case PROC_SERV_REQ_GETRUSAGE:
			
			getrusg = &smsg->proc_service_calls.getrusg;
			smsg->rc = handle_get_thread_resource(getrusg, src);
			rc = lpc_send(src, LPC_INFINITE, &msg);
			kassert( rc == 0 );
			break;
		default:
			smsg->rc = -ENOSYS;
			rc = lpc_send(src, LPC_INFINITE, &msg);
			kassert( rc == 0 );
			break;
		}
	}

	return 0;
}

/** プロセスサービスの初期化
 */
void
proc_service_init(void) {
	int              rc;

	rc = thr_new_thread(&proc_service_thr);
	kassert( rc == 0 );

	rc = thr_create_kthread(proc_service_thr, KSERV_KSERVICE_PRIO, THR_FLAG_NONE,
	    ID_RESV_PROC, handle_proc_service, (void *)ID_RESV_NAME_PROC);
	kassert( rc == 0 );

	rc = thr_start(proc_service_thr, current->tid);
	kassert( rc == 0 );

}