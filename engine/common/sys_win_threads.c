/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "quakedef.h"
#include <sys/types.h>
#include <sys/timeb.h>

#include "winquake.h"
#include <conio.h>

#if (defined(_DEBUG) || defined(DEBUG)) && !defined(NPFTE)
#define CATCHCRASH
LONG CALLBACK nonmsvc_CrashExceptionHandler(PEXCEPTION_POINTERS ExceptionInfo);

#ifdef _MSC_VER	//nt5
PVOID WINAPI AddVectoredExceptionHandler(ULONG FirstHandler, PVECTORED_EXCEPTION_HANDLER VectoredHandler);
#endif
#endif



#if defined(_DEBUG) && defined(_MSC_VER)
const DWORD MS_VC_EXCEPTION=0x406D1388;
#pragma pack(push,8)
typedef struct tagTHREADNAME_INFO
{
	DWORD dwType; // Must be 0x1000.
	LPCSTR szName; // Pointer to name (in user addr space).
	DWORD dwThreadID; // Thread ID (-1=caller thread).
	DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)
void Sys_SetThreadName(unsigned int dwThreadID, char *threadName)
{
	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = threadName;
	info.dwThreadID = dwThreadID;
	info.dwFlags = 0;

	__try
	{
		RaiseException( MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info );
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
	}
}
#endif

#if !defined(WINRT) && defined(MULTITHREAD)
#include <process.h>
/* Thread creation calls */
typedef struct threadwrap_s
{
	int (*func)(void *);
	void *args;
	char name[1];
} threadwrap_t;

typedef struct
{
	HANDLE handle;
	DWORD threadid;
} threadctx_t;

// the thread call is wrapped so we don't need WINAPI everywhere
unsigned int WINAPI threadwrapper(void *args)
{
	threadwrap_t tw;
	tw.func = ((threadwrap_t *)args)->func;
	tw.args = ((threadwrap_t *)args)->args;

#if defined(_DEBUG) && defined(_MSC_VER)
	Sys_SetThreadName(GetCurrentThreadId(), ((threadwrap_t *)args)->name);
#endif
#ifdef CATCHCRASH
	{
		PVOID (WINAPI *pAddVectoredExceptionHandler)(ULONG	FirstHandler,	PVECTORED_EXCEPTION_HANDLER VectoredHandler);
		dllfunction_t dbgfuncs[] = {{(void*)&pAddVectoredExceptionHandler, "AddVectoredExceptionHandler"}, {NULL,NULL}};
		if (Sys_LoadLibrary("kernel32.dll", dbgfuncs) && pAddVectoredExceptionHandler)
			pAddVectoredExceptionHandler(0, nonmsvc_CrashExceptionHandler);
	}
#endif

	free(args);
	tw.func(tw.args);

#ifndef WIN32CRTDLL
	_endthreadex(0);
#endif
	return 0;
}

void *Sys_CreateThread(char *name, int (*func)(void *), void *args, int priority, int stacksize)
{
	threadctx_t *ctx = (threadctx_t *)malloc(sizeof(*ctx));
	threadwrap_t *tw = (threadwrap_t *)malloc(sizeof(threadwrap_t)+strlen(name));

	if (!tw || !ctx)
	{
		free(tw);
		free(ctx);
		return NULL;
	}

	stacksize += 128; // wrapper overhead, also prevent default stack size
	tw->func = func;
	tw->args = args;
	strcpy(tw->name, name);
#ifdef WIN32CRTDLL
	ctx->handle = (HANDLE)CreateThread(NULL, stacksize, &threadwrapper, (void *)tw, 0, &ctx->threadid);
#else
	ctx->handle = (HANDLE)_beginthreadex(NULL, stacksize, &threadwrapper, (void *)tw, 0, &ctx->threadid);
#endif
	if (!ctx->handle)
	{
		free(tw);
		free(ctx);
		return NULL;
	}

	return (void *)ctx;
}

void Sys_DetachThread(void *thread)
{
	threadctx_t *ctx = thread;
	CloseHandle(ctx->handle);
	free(ctx);
}

void Sys_WaitOnThread(void *thread)
{
	threadctx_t *ctx = thread;
	WaitForSingleObject(ctx->handle, INFINITE);
	CloseHandle(ctx->handle);
	free(ctx);
}

//used on fatal errors.
void Sys_ThreadAbort(void)
{
	ExitThread(0);
}

static DWORD mainthread;
void Sys_ThreadsInit(void)
{
	mainthread = GetCurrentThreadId();
}
qboolean Sys_IsMainThread(void)
{
	return mainthread == GetCurrentThreadId();
}

qboolean Sys_IsThread(void *thread)
{
	threadctx_t *ctx = thread;
	return ctx->threadid == GetCurrentThreadId();
}


/* Mutex calls */
/*
Note that a 'mutex' in win32 terminology is a cross-process/kernel object
A critical section is a single-process object, and thus can be provided more cheaply
*/
void *Sys_CreateMutex(void)
{
#ifdef _DEBUG
	//linux's pthread code doesn't like me recursively locking mutexes, so add some debug-only code to catch that on windows too so that we don't get nasty surprises.
	CRITICAL_SECTION *mutex = malloc(sizeof(*mutex)+sizeof(int));
	*(int*)(1+(CRITICAL_SECTION*)mutex) = 0;
#else
	CRITICAL_SECTION *mutex = malloc(sizeof(*mutex));
#endif
	InitializeCriticalSection(mutex);
	return (void *)mutex;
}

/*qboolean Sys_TryLockMutex(void *mutex)
{
#ifdef _DEBUG
	if (!mutex)
	{
		Con_Printf("Invalid mutex\n");
		return false;
	}
#endif
	if (TryEnterCriticalSection(mutex))
	{
#ifdef _DEBUG
		if (*(int*)(1+(CRITICAL_SECTION*)mutex))
			Con_Printf("Double lock\n");
		*(int*)(1+(CRITICAL_SECTION*)mutex)+=1;
#endif
		return true;
	}
	return false;
}*/

qboolean Sys_LockMutex(void *mutex)
{
#ifdef _DEBUG
	if (!mutex)
	{
		Con_Printf("Invalid mutex\n");
		return false;
	}
#endif
	EnterCriticalSection(mutex);
#ifdef _DEBUG
	if (*(int*)(1+(CRITICAL_SECTION*)mutex))
		Con_Printf("Double lock\n");
	*(int*)(1+(CRITICAL_SECTION*)mutex)+=1;
#endif
	return true;
}

qboolean Sys_UnlockMutex(void *mutex)
{
#ifdef _DEBUG
	*(int*)(1+(CRITICAL_SECTION*)mutex)-=1;
#endif
	LeaveCriticalSection(mutex);
	return true;
}

void Sys_DestroyMutex(void *mutex)
{
	DeleteCriticalSection(mutex);
	free(mutex);
}

/* Conditional wait calls */
/*
TODO: Windows Vista has condition variables as documented here:
http://msdn.microsoft.com/en-us/library/ms682052(VS.85).aspx
Note this uses Slim Reader/Writer locks (Vista+ exclusive)
or critical sections.

The condition variable implementation is based on http://www.cs.wustl.edu/~schmidt/win32-cv-1.html.
(the libsdl-based stuff was too buggy)
*/
typedef struct condvar_s
{
	int waiters;
	int release;
	int waitgeneration;
	CRITICAL_SECTION countlock;
	CRITICAL_SECTION mainlock;
	HANDLE evnt;
} condvar_t;

void *Sys_CreateConditional(void)
{
	condvar_t *cv;

	cv = (condvar_t *)malloc(sizeof(condvar_t));
	if (!cv)
		return NULL;

	cv->waiters = 0;
	cv->release = 0;
	cv->waitgeneration = 0;
	InitializeCriticalSection (&cv->mainlock);
	InitializeCriticalSection (&cv->countlock);
	cv->evnt = CreateEvent(NULL, TRUE, FALSE, NULL);

	if (cv->evnt)
		return (void *)cv;

	// something failed so deallocate everything
	DeleteCriticalSection(&cv->countlock);
	DeleteCriticalSection(&cv->mainlock);
	free(cv);

	return NULL;
}

qboolean Sys_LockConditional(void *condv)
{
	EnterCriticalSection(&((condvar_t *)condv)->mainlock);
	return true;
}

qboolean Sys_UnlockConditional(void *condv)
{
	LeaveCriticalSection(&((condvar_t *)condv)->mainlock);
	return true;
}

qboolean Sys_ConditionWait(void *condv)
{
	qboolean done;
	condvar_t *cv = (condvar_t *)condv;
	qboolean success;
	int mygen;

	// increase count for non-signaled waiting threads
	EnterCriticalSection(&cv->countlock);
	cv->waiters++;
	mygen = cv->waitgeneration;
	LeaveCriticalSection(&cv->countlock);

	LeaveCriticalSection(&cv->mainlock); // unlock as per condition variable definition

	// wait on a signal
	for(;;)
	{
#if 1
		success = (WaitForSingleObject(cv->evnt, INFINITE) != WAIT_FAILED);
#else
		do
		{
			MSG msg;
			while (PeekMessage (&msg, NULL, 0, 0, PM_REMOVE))
				DispatchMessage (&msg);
			status = MsgWaitForMultipleObjects(1, &cv->evnt, FALSE, INFINITE, QS_SENDMESSAGE|QS_POSTMESSAGE);
		} while (status == (WAIT_OBJECT_0+1));
		success = status != WAIT_FAILED;
#endif
		EnterCriticalSection(&cv->countlock);
		done = cv->release > 0 && cv->waitgeneration != mygen;
		LeaveCriticalSection(&cv->countlock);
		if (done)
			break;
	}

	EnterCriticalSection(&cv->mainlock); // lock as per condition variable definition

	// update waiting count and alert signaling thread that we're done to avoid the deadlock condition
	EnterCriticalSection(&cv->countlock);
	cv->waiters--;
	cv->release--;
	done = cv->release == 0;
	LeaveCriticalSection(&cv->countlock);

	if (done)
		ResetEvent(cv->evnt);

	return true;
}

qboolean Sys_ConditionSignal(void *condv)
{
	condvar_t *cv = (condvar_t *)condv;

	EnterCriticalSection(&cv->mainlock);

	// if there are non-signaled waiting threads, we signal one and wait on the response
	EnterCriticalSection(&cv->countlock);
	if (cv->waiters > cv->release)
	{
		SetEvent(cv->evnt);
		cv->release++;
		cv->waitgeneration++;
	}
	LeaveCriticalSection(&cv->countlock);

	LeaveCriticalSection(&cv->mainlock);

	return true;
}

qboolean Sys_ConditionBroadcast(void *condv)
{
	condvar_t *cv = (condvar_t *)condv;

	EnterCriticalSection(&cv->mainlock);

	// if there are non-signaled waiting threads, we signal all of them and wait on all the responses back
	EnterCriticalSection(&cv->countlock);
	if (cv->waiters > 0)
	{
		SetEvent(cv->evnt);
		cv->release = cv->waiters;
		cv->waitgeneration++;
	}
	LeaveCriticalSection(&cv->countlock);

	LeaveCriticalSection(&cv->mainlock);

	return true;
}

void Sys_DestroyConditional(void *condv)
{
	condvar_t *cv = (condvar_t *)condv;

	//make sure noone is still trying to poke it while shutting down
//	Sys_LockConditional(condv);
//	Sys_UnlockConditional(condv);

	CloseHandle(cv->evnt);
	DeleteCriticalSection(&cv->countlock);
	DeleteCriticalSection(&cv->mainlock);
	free(cv);
}

#endif

#ifdef SUBSERVERS
typedef struct slaveserver_s
{
	pubsubserver_t pub;

	HANDLE inpipe;
	HANDLE outpipe;
	
	qbyte inbuffer[2048];
	int inbufsize;
} winsubserver_t;


pubsubserver_t *Sys_ForkServer(void)
{
	char exename[256];
	char curdir[256];
	char cmdline[8192];
	PROCESS_INFORMATION childinfo;
	STARTUPINFO startinfo;
	SECURITY_ATTRIBUTES pipesec = {sizeof(pipesec), NULL, TRUE};
	winsubserver_t *ctx = Z_Malloc(sizeof(*ctx));

	GetModuleFileName(NULL, exename, sizeof(exename));
	GetCurrentDirectory(sizeof(curdir), curdir);
	Q_snprintfz(cmdline, sizeof(cmdline), "foo -noreset -clusterslave %s", FS_GetManifestArgs());	//fixme: include which manifest is in use, so configs get set up the same.

	memset(&startinfo, 0, sizeof(startinfo));
	startinfo.cb = sizeof(startinfo);
	startinfo.hStdInput = NULL;
	startinfo.hStdError = NULL;
	startinfo.hStdOutput = NULL;
	startinfo.dwFlags |= STARTF_USESTDHANDLES;

	//create pipes for the stdin/stdout.
	CreatePipe(&ctx->inpipe, &startinfo.hStdOutput, &pipesec, 0);
	CreatePipe(&startinfo.hStdInput, &ctx->outpipe, &pipesec, 0);

	SetHandleInformation(ctx->inpipe, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(ctx->outpipe, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(startinfo.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	SetHandleInformation(startinfo.hStdInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

	CreateProcess(exename, cmdline, NULL, NULL, TRUE, 0, NULL, curdir, &startinfo, &childinfo);

	//these ends of the pipes were inherited by now, so we can discard them in the caller.
	CloseHandle(startinfo.hStdOutput);
	CloseHandle(startinfo.hStdInput);
	return &ctx->pub;
}

void Sys_InstructSlave(pubsubserver_t *ps, sizebuf_t *cmd)
{
	winsubserver_t *s = (winsubserver_t*)ps;
	DWORD written = 0;
	cmd->data[0] = cmd->cursize & 0xff;
	cmd->data[1] = (cmd->cursize>>8) & 0xff;
	WriteFile(s->outpipe, cmd->data, cmd->cursize, &written, NULL);
}

void SSV_InstructMaster(sizebuf_t *cmd)
{
	HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD written = 0;
	cmd->data[0] = cmd->cursize & 0xff;
	cmd->data[1] = (cmd->cursize>>8) & 0xff;
	WriteFile(output, cmd->data, cmd->cursize, &written, NULL);
}

int Sys_SubServerRead(pubsubserver_t *ps)
{
	DWORD avail;
	winsubserver_t *s = (winsubserver_t*)ps;

	if (!PeekNamedPipe(s->inpipe, NULL, 0, NULL, &avail, NULL))
	{
		CloseHandle(s->inpipe);
		CloseHandle(s->outpipe);
		Con_Printf("%i:%s has died\n", s->pub.id, s->pub.name);
		return -1;
	}
	else if (avail)
	{
		if (avail > sizeof(s->inbuffer)-1-s->inbufsize)
			avail = sizeof(s->inbuffer)-1-s->inbufsize;
		if (ReadFile(s->inpipe, s->inbuffer+s->inbufsize, avail, &avail, NULL))
			s->inbufsize += avail;
	}

	if(s->inbufsize >= 2)
	{
		unsigned short len = s->inbuffer[0] | (s->inbuffer[1]<<8);
		if (s->inbufsize >= len && len>=2)
		{
			memcpy(net_message.data, s->inbuffer+2, len-2);
			net_message.cursize = len-2;
			memmove(s->inbuffer, s->inbuffer+len, s->inbufsize - len);
			s->inbufsize -= len;
			MSG_BeginReading (msg_nullnetprim);

			return 1;
		}
	}
	return 0;
}
#endif

