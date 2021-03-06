#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <malloc.h>
#include <memory.h>
#include <sys/time.h>
#include "scheduler.h"

// task switching시 저장되어야 하는 정보
struct frame {
	unsigned long flags;
	unsigned long ebp;
	unsigned long edi;
	unsigned long esi;
	unsigned long edx;
	unsigned long ecx;
	unsigned long ebx;
	unsigned long eax;
	unsigned long retaddr;
	unsigned long retaddr2;
	unsigned long data;
};

typedef struct sch_handle_tag {
	int child_task;

	TaskInfo running_task;
	TaskInfo root_task;
} SchHandle;

// global schedule handler
SchHandle gh_sch;

// task data struct
TaskInfo task_get_runningtask();
void task_insert(TaskInfo taskinfo);
void task_delete(TaskInfo taskinfo);
void task_next();
void scheduler();
void parent_task();

// thread_create : task를 생성하는 함수로 taskinfo 구조체를 할당하고 구성한다
TaskInfo thread_create(TaskFunc callback, void *context)
{
	TaskInfo taskinfo;
	// task를 위한 공간 할당
	taskinfo = malloc(sizeof(*taskinfo));
	memset(taskinfo, 0x00, sizeof(*taskinfo));

	{
		struct frame *f = (struct frame *)&taskinfo->stack[
			THREAD_STACKSIZE - sizeof(struct frame)/4];
		// taskinfo로할당된 공간 중 stack부분 뒤쪽에 frame을 위한 공간으
		// 로 할당 이어 task가 수행되면서 stack공간을 활용
		int i;
		for(i = 0; i < THREAD_STACKSIZE; ++i) // stack overfllow check
			taskinfo->stack[i] = i;
		memset(f, 0, sizeof(struct frame));
		f->retaddr = (unsigned long)callback;
		f->retaddr2 = (unsigned long)thread_kill;
		f->data = (unsigned long)context;
		taskinfo->sp = (unsigned long)&f->flags;
		f->ebp = (unsigned long)&f->eax;
	}
	// task 생성에 따라 gh_sch에 child task가 늘었을을 표시
	gh_sch.child_task++;
	// gh_sch.child_task 값으로 task_id 할당
	taskinfo->task_id = gh_sch.child_task;
	// task 생성시 TASK_READY로 상태를 설정함
	taskinfo->status = TASK_READY;
	// taskinfo구조체들의 linkedlist에 새 thread의 taskinfo 구조체를 삽입
	task_insert(taskinfo);

	return taskinfo;
}

/* thread_init : 초기화 함수로 main 함수가 처음에 호출하여
 * global scheduler handeler를 초기화 하고, parent_task를 생성한다
 */
void thread_init()
{
	gh_sch.root_task = NULL;
	gh_sch.running_task = NULL;

	gh_sch.child_task = 0;
	
	thread_create(parent_task, NULL);
}

/*
 * thread_switch : 수행중이던 task가 다른 대기중인 task에게 cpu사용을 양보하게
 * 하는 함수로, 현재 cpu레지스터의 값이 수행중이던 task의 stack부분에 차례차례
 * 저장되게 되며, 다음에 수행될 것으로 선택된 task의 taskinfo의 stack정보가 레지
 * 스터로 올려진다.
 */
static unsigned long spsave, sptmp;
void thread_switch(int a)
{
	asm(	"push %%rax\n \t"
		"push %%rbx\n \t"
		"push %%rcx\n \t"
		"push %%rdx\n \t"
		"push %%rsi\n \t"
		"push %%rdi\n \t"
		"push %%rbp\n \t"
		"push %%rbp\n \t"
		"mov  %%rsp, %0"
		: "=r" (spsave)
	   );

	gh_sch.running_task->sp = spsave;

	scheduler();
	sptmp = gh_sch.running_task->sp;

	asm(	"mov %0, %%rsp\n \t"
		"pop %%rbp\n \t"
		"pop %%rbp\n \t"
		"pop %%rdi\n \t"
		"pop %%rsi\n \t"
		"pop %%rdx\n \t"
		"pop %%rcx\n \t"
		"pop %%rbx\n \t"
		"pop %%rax\n \t"
		::"r" (sptmp)
	   );
}

// 다음 수행될 task를 선택하는 함수
void scheduler(void)
{
	TaskInfo task;
	// gh_sch의 running_task가 가르키고 있는 taskinfo 받음
	task = task_get_runningtask();
	
	switch( task->status ) {
		// task 상태가 TASK_RUN이나 TASK_SLEEP이면 선택됨
		case TASK_RUN:
		case TASK_SLEEP:
			break;
		// task상태가 TASK_KILL이면 delete하고, swiching함수 다시 호출
		case TASK_KILL:
			task_delete(task);
			scheduler();
			break;
		// task상태가 TASK_YIELD이면 상태를 TASK_RUN으로 바꾸고 선택됨
		case TASK_YIELD:
			task->status = TASK_RUN;
			break;
		// task상태가 TASK_READY이면 상태를 TASK_RUN으로 바꾸고 선택됨
		case TASK_READY:
			task->status = TASK_RUN;
			break;
	}
	// gh_sch의 running_task를 linkedlist의 다음 task로 설정
	task_next();
}

void thread_wait(void)
{
	parent_task(NULL);
}

// task 상태를 TASK_KILL로 설정 후, thread_yield
void thread_kill(void)
{
	TaskInfo	task;
	task = task_get_runningtask();
	task->status = TASK_KILL;
	thread_switch(0);
}

void thread_uninit(void)
{
	return;
}

// child thread가 더이상 없을때까지 thread_switch
void parent_task(void *context)
{
	// signal 처리를 위한 정보를 위한 구조체
	struct sigaction act;
	sigset_t masksets;
	pid_t pid;

	// signal set 초기화
	sigemptyset( &masksets );
	// signal handler로 thread_switch() 등록
	act.sa_handler = thread_switch;
	act.sa_mask = masksets;
	act.sa_flags = SA_NODEFER;

	// signal 수신 때 취할 action 설정
	sigaction( SIGUSR1, &act, NULL );

	if( ( pid = fork() ) == 0 ) {
		while(1) {
			sleep(1);
			kill( getppid(), SIGUSR1 );
		}
	} else {
		while(1) {
			// child_task가 1개 남았을 때, 즉,parent_task만 남았을때
			if( gh_sch.child_task == 1 ) {
				kill( pid, SIGINT );
				break;
			}
		}
	}
}

// linkedlist에 새로운 taskinfo 삽입
void task_insert(TaskInfo taskinfo)
{
	if( gh_sch.root_task == NULL ){
		gh_sch.root_task = taskinfo;
		gh_sch.running_task = taskinfo;
	} else {
		TaskInfo temp = gh_sch.root_task;
		while( temp->next != NULL )
			temp = temp->next;
		temp->next = taskinfo;
		taskinfo->prev = temp;
	}
}

// linkedlist에 gh_sch.running_task가 가르키고 있는 task 리턴
TaskInfo task_get_runningtask(void)
{
	return gh_sch.running_task;
}

// linkedlist에서 gh_sch.running_task가 가르키고 있는 task의 다음 task리턴
void task_next(void)
{
	TaskInfo temp;
	temp = gh_sch.running_task;
	// gh_sch.running_task가 NULL이 아니면
	if( temp->next != NULL )
		gh_sch.running_task = temp->next;
	// gh_sch.running_task가 NULL이면, parent task를 가르킴
	else 
		gh_sch.running_task = gh_sch.root_task;
}

// linkedlist에서 task를 지움
void task_delete(TaskInfo taskinfo)
{
	TaskInfo temp = taskinfo->prev;
	if( gh_sch.root_task == taskinfo ) {
		gh_sch.root_task = NULL;
		gh_sch.running_task = NULL;
		gh_sch.child_task = 0;
	} else {
		temp->next = taskinfo->next;

		if( taskinfo == gh_sch.running_task ) {
			if( temp->next != NULL ){
				(taskinfo->next)->prev = temp;
				gh_sch.running_task = temp->next;
			} else {
				gh_sch.running_task = temp;
			}
		}
		gh_sch.child_task--;
	}
	free(taskinfo);
}
