#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "tinycthread/tinycthread.h"

#if __unix__
	#include <signal.h>
	#include <sys/mman.h>
#if __linux__
	#include <sys/prctl.h>
#endif
#endif

#if DRIFT_MODULES
	#include <SDL.h>
#endif

#include "tracy/TracyC.h"

// #define _TINA_MUTEX_T mtx_t
// #define _TINA_MUTEX_INIT(_LOCK_) mtx_init(&_LOCK_, mtx_plain)
// #define _TINA_MUTEX_DESTROY(_LOCK_) mtx_destroy(&_LOCK_)
// #define _TINA_MUTEX_LOCK(_LOCK_) {TracyCZoneN(ZONE, "Lock", true); mtx_lock(&_LOCK_); TracyCZoneEnd(ZONE);}
// #define _TINA_MUTEX_UNLOCK(_LOCK_) {TracyCZoneN(ZONE, "Unlock", true); mtx_unlock(&_LOCK_); TracyCZoneEnd(ZONE);}
// #define _TINA_COND_T cnd_t
// #define _TINA_COND_INIT(_SIG_) cnd_init(&_SIG_)
// #define _TINA_COND_DESTROY(_SIG_) cnd_destroy(&_SIG_)
// #define _TINA_COND_WAIT(_SIG_, _LOCK_) cnd_wait(&_SIG_, &_LOCK_);
// #define _TINA_COND_SIGNAL(_SIG_) {TracyCZoneN(ZONE, "Signal", true); cnd_signal(&_SIG_); TracyCZoneEnd(ZONE);}
// #define _TINA_COND_BROADCAST(_SIG_) cnd_broadcast(&_SIG_)

#define TINA_IMPLEMENTATION
#include "tina/tina.h"

#ifdef TRACY_FIBERS
#define _TINA_PROFILE_ENTER(_JOB_) TracyCFiberEnter(job->fiber->name)
#define _TINA_PROFILE_LEAVE(_JOB_, _STATUS) TracyCFiberLeave
#endif

#define TINA_JOBS_IMPLEMENTATION
#include "tina/tina_jobs.h"

#include "drift_base.h"

void DriftThrottledParallelFor(tina_job* job, const char* name, tina_job_func func, void* user_data, uint count){
	tina_scheduler* sched = tina_job_get_scheduler(job);
	tina_group jobs = {};
	for(uint idx = 0; idx < count; idx++){
		tina_job_wait(job, &jobs, DRIFT_APP_MAX_THREADS);
		tina_scheduler_enqueue(sched, name, func, user_data, idx, DRIFT_JOB_QUEUE_WORK, &jobs);
	}
	tina_job_wait(job, &jobs, 0);
}

#if DRIFT_MODULES
static void DriftAppModuleLoad(tina_job* job){
	DriftApp* app = tina_job_get_description(job)->user_data;
	
	app->module_status = DRIFT_APP_MODULE_READY;
	tina_job_wait(job, &app->module_entrypoint_jobs, 0);
	SDL_UnloadObject(app->module);
	
	char libname[256];
#if __unix__
	snprintf(libname, sizeof(libname), "./%s.so", app->module_libname);
#elif __APPLE__
	snprintf(libname, sizeof(libname), "./%s.dylib", app->module_libname);
#elif __WIN64__
	snprintf(libname, sizeof(libname), "%s-tmp.dll", app->module_libname);
	// Windows can't replace an open file. So need to copy the lib before opening it.
	char srcname[256];
	snprintf(srcname, sizeof(srcname), "%s.dll", app->module_libname);
	DRIFT_ASSERT_HARD(CopyFile(srcname, libname, false), "Failed to copy lib");
#else
	#error Unhandled platform.
#endif
	
	while((app->module = SDL_LoadObject(libname)) == NULL){
		DRIFT_LOG("Failed to load module. (%s)\nRebuild and press the any key to try again.", SDL_GetError());
		getchar();
	}
	
	tina_job_func* init = SDL_LoadFunction(app->module, app->module_entrypoint);
	DRIFT_ASSERT_HARD(init, "Failed to find init function. (%s)", SDL_GetError());
	
	app->module_status = DRIFT_APP_MODULE_IDLE;
	tina_scheduler_enqueue(app->scheduler, "JobAppModuleInit", init, app, 0, DRIFT_JOB_QUEUE_MAIN, &app->module_entrypoint_jobs);
}

tina_job_func* DriftAppModuleStart = DriftAppModuleLoad;

static void ModuleRebuild(tina_job* job){
	DriftApp* app = tina_job_get_description(job)->user_data;
	while(system(app->module_build_command)){
		app->module_status = DRIFT_APP_MODULE_ERROR;
		DRIFT_LOG("Build failed. Press any key to try again.");
		getchar();
	}
	
	tina_scheduler_enqueue(app->scheduler, "JobAppModuleLoad", DriftAppModuleLoad, app, 0, DRIFT_JOB_QUEUE_WORK, NULL);
}

bool DriftAppModuleRequestReload(DriftApp* app, tina_job* job){
	switch(app->module_status){
		case DRIFT_APP_MODULE_IDLE:
			app->module_status = DRIFT_APP_MODULE_BUILDING;
			tina_scheduler_enqueue(app->scheduler, "JobModuleRebuild", ModuleRebuild, app, 0, DRIFT_JOB_QUEUE_WORK, &app->module_rebuild_jobs);
			return false;
		case DRIFT_APP_MODULE_ERROR:
			tina_job_wait(job, &app->module_rebuild_jobs, 0);
			return false;
		case DRIFT_APP_MODULE_READY:
			return true;
		default: return false;
	}
}
#endif

#if __unix__ || __APPLE__
	#include <unistd.h>
	static unsigned DriftAppGetCPUCount(void){return sysconf(_SC_NPROCESSORS_ONLN);}
#elif __WIN64__
	static unsigned DriftAppGetCPUCount(void){
		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);
		return sysinfo.dwNumberOfProcessors;
	}
#else
	#error Not implemented for this platform.
#endif

static _Thread_local uint ThreadID;
uint DriftAppGetThreadID(void){return ThreadID;}

static int DriftAppWorkerThreadBody(void* user_data){
	DriftThread* thread = user_data;
	ThreadID = thread->id;
	
#if __linux__
	sigset_t mask;
	sigfillset(&mask);
	DRIFT_ASSERT(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0, "Failed to block signals");
#endif

#if __linux__
	prctl(PR_SET_NAME, thread->name);
#endif
	
#if TRACY_ENABLE
	TracyCSetThreadName(thread->name);
#endif
	
	DriftApp* app = thread->app;
	tina_scheduler_run(app->scheduler, thread->queue, false);
	
	return 0;
}

static void* DriftAppFiberBody(tina* fiber, void* value){
	while(true){
		tina_job* job = value;
		
		// TracyCZoneN(ZONE_JOB, "DriftJob", true);
		// TracyCZoneName(ZONE_JOB, job->desc.name, strlen(job->desc.name));
		// TracyCZoneValue(ZONE_JOB, job->desc.user_idx);
		job->desc.func(job);
		// TracyCZoneEnd(ZONE_JOB);
		
		value = tina_yield(fiber, (void*)_TINA_STATUS_COMPLETED);
	}
	
	DRIFT_ABORT("Unreachable");
}

static tina* DriftAppFiberFactory(tina_scheduler* sched, unsigned fiber_idx, void* buffer, size_t stack_size, void* user_ptr){
	stack_size = 256*1024;
	
#if __unix__
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK;
	buffer = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, flags, -1, 0);
	
	// Might as well add guard pages for predictable crashes.
	size_t page_size = getpagesize();
	stack_size -= page_size;
	mprotect((u8*)buffer + page_size, page_size, PROT_NONE);
	mprotect((u8*)buffer + stack_size, page_size, PROT_NONE);
#else
	buffer = DriftAlloc(DriftSystemMem, stack_size);
#endif

	tina* fiber = tina_init(buffer, stack_size, DriftAppFiberBody, sched);
	fiber->name = DriftSMPrintf(DriftSystemMem, "TINA JOBS FIBER %02d", fiber_idx);;
	
	return fiber;
}

static void DriftThreadInit(DriftThread* thread, DriftApp* app, uint id, uint queue, const char* name){
	*thread = (DriftThread){.app = app, .id = id, .queue = queue, .name = name};
	thrd_create(&thread->thread, DriftAppWorkerThreadBody, thread);
}

int DriftMain(DriftApp* app){
	DriftStopwatch sw = DRIFT_STOPWATCH_START("App init");
	
	// Setup jobs.
	uint cpu_count = DriftAppGetCPUCount();
	app->thread_count = DRIFT_MAX(DRIFT_MIN(cpu_count + 1u, DRIFT_APP_MAX_THREADS), 3u);
	DRIFT_LOG("DriftApp thread count: %d", app->thread_count);
	
	uint job_count = 1024, fiber_count = 64;
	size_t sched_size = tina_scheduler_size(job_count, _DRIFT_JOB_QUEUE_COUNT, fiber_count, 0);
	void* sched_buffer = DriftAlloc(DriftSystemMem, sched_size);
	app->scheduler = _tina_scheduler_init2(sched_buffer, job_count, _DRIFT_JOB_QUEUE_COUNT, fiber_count, 0, DriftAppFiberFactory, NULL);
	tina_scheduler_queue_priority(app->scheduler, DRIFT_JOB_QUEUE_MAIN, DRIFT_JOB_QUEUE_WORK);
	
	DriftThreadInit(app->threads + 1, app, DRIFT_THREAD_ID_GFX, DRIFT_JOB_QUEUE_GFX, "DriftGFXThread");
	for(uint i = DRIFT_THREAD_ID_WORKER0; i < app->thread_count; i++){
		const char* name = DriftSMPrintf(DriftSystemMem, "DriftWorkerThread %d", i);
		DriftThreadInit(app->threads + i, app, i, DRIFT_JOB_QUEUE_WORK, name);
	}
	DRIFT_STOPWATCH_MARK(sw, "jobs");
	
	// Setup memory.
	app->zone_heap = DriftZoneMemHeapNew("Global", DriftSystemMem, 1 << 20, 256, 8);
	DRIFT_STOPWATCH_MARK(sw, "mem");
	
	// Start shell and module.
	app->shell_func(app, DRIFT_SHELL_START, NULL);
	tina_scheduler_enqueue(app->scheduler, "JobAppInit", app->init_func, app, 0, DRIFT_JOB_QUEUE_MAIN, NULL);
	// DRIFT_STOPWATCH_STOP(sw, "shell");
	
	// Run the main queue until shutdown.
	tina_scheduler_run(app->scheduler, DRIFT_JOB_QUEUE_MAIN, false);
	
	// Gracefully shut down worker threads.
	for(uint i = DRIFT_THREAD_ID_GFX; i < app->thread_count; i++) thrd_join(app->threads[i].thread, NULL);
	
	app->shell_func(app, DRIFT_SHELL_STOP, NULL);
	
	if(app->shell_restart){
		DRIFT_LOG("Restarting...");
		app->shell_func = app->shell_restart;
		app->shell_restart = NULL;
		return DriftMain(app);
	} else {
		return EXIT_SUCCESS;
	}
}

void DriftAppShowWindow(DriftApp* app){
	DriftAppAssertMainThread();
	app->shell_func(app, DRIFT_SHELL_SHOW_WINDOW, NULL);
}

void DriftAppHaltScheduler(DriftApp* app){
	tina_scheduler_interrupt(app->scheduler, DRIFT_JOB_QUEUE_MAIN);
	tina_scheduler_interrupt(app->scheduler, DRIFT_JOB_QUEUE_WORK);
	tina_scheduler_interrupt(app->scheduler, DRIFT_JOB_QUEUE_GFX);
}

DriftGfxRenderer* DriftAppBeginFrame(DriftApp* app, DriftZoneMem* zone){
	DriftAppAssertMainThread();
	return app->shell_func(app, DRIFT_SHELL_BEGIN_FRAME, zone);
}

void DriftAppPresentFrame(DriftApp* app, DriftGfxRenderer* renderer){
	DriftAppAssertGfxThread();
	app->shell_func(app, DRIFT_SHELL_PRESENT_FRAME, renderer);
}

void DriftAppToggleFullscreen(DriftApp* app){
	DriftAppAssertMainThread();
	app->fullscreen = !app->fullscreen;
	app->shell_func(app, DRIFT_SHELL_TOGGLE_FULLSCREEN, NULL);
}

void DriftAppAssertMainThread(void){
	DRIFT_ASSERT_HARD(ThreadID == DRIFT_THREAD_ID_MAIN, "Must be called from the main queue.");
}

void DriftAppAssertGfxThread(void){
	DRIFT_ASSERT_HARD(ThreadID == DRIFT_THREAD_ID_GFX, "Must be called from the gfx queue.");
}

void* DriftShellConsole(DriftApp* app, DriftShellEvent event, void* shell_value){
	switch(event){
		case DRIFT_SHELL_START:{
			DRIFT_LOG("Using Console");
		} break;
		
		case DRIFT_SHELL_STOP:{
			DRIFT_LOG("Console Shutdown.");
		} break;
		
		default: break;
	}
	
	return NULL;
}
