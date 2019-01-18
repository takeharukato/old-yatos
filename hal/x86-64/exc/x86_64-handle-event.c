/* -*- mode: C; coding:utf-8 -*- */
/**********************************************************************/
/*  Yet Another Teachable Operating System                            */
/*  Copyright 2016 Takeharu KATO                                      */
/*                                                                    */
/*  Post exception handling routines                                  */
/*                                                                    */
/**********************************************************************/
#include <stddef.h>

#include <kern/config.h>
#include <kern/assert.h>
#include <kern/string.h>
#include <kern/kprintf.h>
#include <kern/spinlock.h>
#include <kern/proc.h>
#include <kern/vm.h>
#include <kern/thread.h>
#include <kern/thread-info.h>
#include <kern/sched.h>
#include <kern/errno.h>
#include <kern/async-event.h>
#include <kern/page.h>
#include <kern/irq.h>

#include <hal/arch-cpu.h>

/** イベントハンドラからの復帰
    @param[in] user_ef  ユーザ空間中のイベントフレーム
    @param[in] ctx      割込みコンテキスト
    @note HAL内部から呼ばれる関数
 */
void
_x86_64_return_from_event_handler(event_frame *user_ef, trap_context *ctx) {
	int rc;
	event_frame ef;

	rc = vm_copy_in(&current->p->vm, &ef, user_ef, sizeof(event_frame));
	if ( rc == -EFAULT )
		goto exit_out;

	/*  コンテキストを復元      */
	memcpy( ctx, &ef.trap_ctx, sizeof(trap_context) );
	/*  FPU コンテキストを復元  */
	if ( current->ti->arch_flags & TI_X86_64_FPU_USED )
		x86_64_fpuctx_restore( &ef.fpu_frame ); 
	else
		memcpy( &current->fpctx, &ef.fpu_frame, sizeof(fpu_context) ); 
	return;

exit_out:
	thr_exit(-EFAULT);
	return;
}
/** イベントハンドラ呼出しフレーム作成
    @param[in] ctx 割込みコンテキスト
 */
void
x86_64_setup_event_handler(trap_context *ctx) {
	int                  rc;
	event_id             id;
	event_node       *newev;
	event_frame    *user_ef;
	event_frame          ef;
	intrflags         flags;

	/*
	 * 捕捉可能なシグナルを取り出す
	 */
	do{
		rc = ev_dequeue(&newev);
		if ( rc != 0 )
			return;  /* イベントが来ていない  */
		
		rc = kcom_handle_system_event(newev);  /*  捕捉不能イベントを処理する  */

	} while( rc == 0 );
		
	if ( current->p->u_evhandler == NULL ) {
		
		/* イベントハンドラを持たないプロセスの場合は, 
		 * イベント呼出しを行わずイベントを破棄する。
		 * CPU例外送出イベントの場合は, 強制終了させる。
		 */

		id = newev->info.no;

		kfree(newev);
		if ( ev_is_cpu_exception(id) ) 
			goto exit_out;
		
		return;
	}

	/*
	 * ユーザスタック上にイベント用フレームを作成
	 * Note: カーネルのパーソナリティへの依存性を排除するために,
	 *       yatosはユーザランドでイベントハンドラのディスパッチを行う
	 *       このためRed Zone分のスタック伸長を行う必要はない。
	 *
	 *       [背景となるアーキテクチャ仕様]
	 *       x86-64のABIでは, シグナルハンドラや割込みハンドラは, Red Zoneを
	 *       更新してはならないことを規定している。
	 *
	 *       Red Zoneは, アプリケーション中のリーフ関数が使用することを想定した
	 *       予約領域である。 RBPをベースポインタとして使用する場合に, 
	 *       Red Zone領域をデータ領域として使用してもRBPの値からスタックの
	 *       値を復元できるため, 当該領域を関数エントリ時のRSPより上位のアドレスに
	 *       確保しても関数呼び出しを正しく行え, かつ, 関数復帰時のスタック
	 *       フレームの復元処理命令を2命令削減可能となる
	 *       ( AMD64 ABI Draft 0.99.5 3.2.2 The Stack Frame 参照 )。
	 *
	 *       [仕様策定の背景]
	 *       yatosはユーザランドでイベントハンドラのディスパッチを行う設計
	 *       を採用しているため, 例外復帰先がリーフ関数になることはない。 このため,
	 *       Red Zone分を事前に割り当てることによるページフォルトコストの削減効果と
	 *       使用されないページが割り当てられることによるメモリ消費量増加との
	 *       トレードオフからRed Zone分の伸長を行わない方針とした。
	 */
	user_ef = (event_frame *)( (void *)ctx->rsp - sizeof(event_frame) );
	rc= proc_expand_stack(current->p, (void *)user_ef);
	if ( rc != 0 ) {
			
		kprintf(KERN_INF, 
		    "Stack exhausted KILLED current=%p tid=%d rc=%d frame=%p\n", 
		    current, current->tid, rc, user_ef);
		rc = -EFAULT;
		goto exit_out;
	}

	spinlock_lock_disable_intr( &current->p->lock, &flags );

	memset( &ef, 0, sizeof(event_frame) );

	memcpy( &ef.info, &newev->info, sizeof(evinfo) );
	memcpy( &ef.trap_ctx, ctx, sizeof(trap_context) );  /*  コンテキストをコピー  */

	/*  FPUコンテキストをコピー  */
	if ( current->ti->arch_flags & TI_X86_64_FPU_USED )
		x86_64_fpuctx_save( &ef.fpu_frame );
	else
		memcpy( &ef.fpu_frame,  &current->fpctx, sizeof(fpu_context) ); 

	/*
	 * ユーザスタック上にフレーム情報を書き込み
	 */
	rc = vm_copy_out(&current->p->vm, user_ef, &ef, sizeof(event_frame));
	if ( rc == -EFAULT )
		goto unlock_out;

	spinlock_unlock_restore_intr( &current->p->lock, &flags );

	/*
	 * ユーザ区間のハンドラに制御を移すように割込みコンテキストを書換え
	 */
	ctx->rip = (uint64_t)current->p->u_evhandler;
	ctx->rsp = (uint64_t)user_ef;
	ctx->rdi = (uint64_t)newev->info.no;
	ctx->rsi = (uint64_t)&user_ef->info;
	ctx->rdx = (uint64_t)user_ef;

	kfree(newev);  /*  イベントノードの解放  */

	return;

unlock_out:
	spinlock_unlock_restore_intr( &current->p->lock, &flags );

exit_out:
	kfree(newev);  /*  イベントノードの解放  */
	thr_exit(rc);   /*  自スレッド終了  */	
	/*  ここにはこない  */
}

/** 非同期イベント/遅延ディスパッチを処理する
    @param[in] ctx 例外コンテキスト
 */
void
x86_64_handle_post_exception(trap_context  __attribute__ ((unused))   *ctx) {

	while ( ti_dispatch_delayed( ti_get_current_tinfo() ) ) 
		sched_schedule();   /*  遅延ディスパッチを処理  */

	if ( hal_is_intr_from_user(ctx) )
		x86_64_setup_event_handler(ctx);
}
