/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  全局变量声明
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/globals.c
 *
 * NOTES
 *	  各处都会用到的全局变量应在此处声明，而不要放在其他模块中。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/file_perm.h"
#include "libpq/libpq-be.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "storage/backendid.h"


ProtocolVersion FrontendProtocol;

volatile sig_atomic_t InterruptPending = false;
volatile sig_atomic_t QueryCancelPending = false;
volatile sig_atomic_t ProcDiePending = false;
volatile sig_atomic_t ClientConnectionLost = false;
volatile sig_atomic_t IdleInTransactionSessionTimeoutPending = false;
volatile sig_atomic_t ProcSignalBarrierPending = false;
volatile sig_atomic_t LogMemoryContextPending = false;
volatile uint32 InterruptHoldoffCount = 0;
volatile uint32 QueryCancelHoldoffCount = 0;
volatile uint32 CritSectionCount = 0;

int			MyProcPid;
pg_time_t	MyStartTime;
TimestampTz MyStartTimestamp;
struct Port *MyProcPort;
int32		MyCancelKey;
int			MyPMChildSlot;

/*
 * MyLatch 指向当前进程应用于信号处理的 latch。如果当前进程此刻没有
 * PGPROC 条目，它指向进程本地的 latch；如果有，则指向 PGPROC->procLatch。
 * 因此它总是可以在信号处理函数中使用，无需检查其是否存在。
 */
struct Latch *MyLatch;

/*
 * DataDir 是 PGDATA 目录树顶层目录的绝对路径。除早期启动阶段之外，
 * 它也是服务器的工作目录；因此大多数代码都可以简单地使用相对路径，
 * 而不必显式引用 DataDir。
 */
char	   *DataDir = NULL;

/*
 * 数据目录的模式。默认为 0700，但如果数据目录实际就是 0750 模式，
 * checkDataDir() 可能会将其改为 0750。
 */
int			data_directory_mode = PG_DIR_MODE_OWNER;

char		OutputFileName[MAXPGPATH];	/* 调试输出文件 */

char		my_exec_path[MAXPGPATH];	/* 我的可执行文件的完整路径 */
char		pkglib_path[MAXPGPATH]; /* lib 目录的完整路径 */

BackendId	MyBackendId = InvalidBackendId;

BackendId	ParallelLeaderBackendId = InvalidBackendId;

Oid			MyDatabaseId = InvalidOid;

Oid			MyDatabaseTableSpace = InvalidOid;

/*
 * DatabasePath 是当前数据库主目录（即在默认表空间中的目录）的路径
 * （相对于 DataDir）。
 */
char	   *DatabasePath = NULL;

pid_t		PostmasterPid = 0;

/*
 * IsPostmasterEnvironment 在 postmaster 进程及任何 postmaster 子进程中为
 * true；在独立进程（bootstrap 或独立后端）中为 false。
 * IsUnderPostmaster 在 postmaster 子进程中为 true。注意，"子进程"包括
 * 所有子进程，而不仅限于普通后端。这些变量应尽可能在进程执行的早期
 * 设置正确，这样如果进程初始化期间发生错误，错误处理才能做出正确的
 * 行为。
 *
 * 这些变量是针对 bootstrap/独立运行场景初始化的。
 */
bool		IsPostmasterEnvironment = false;
bool		IsUnderPostmaster = false;
bool		IsBinaryUpgrade = false;

bool		ExitOnAnyError = false;

int			IntervalStyle = INTSTYLE_POSTGRES;

bool		enableFsync = true;
bool		allowSystemTableMods = false;
int			work_mem = 4096;
double		hash_mem_multiplier = 1.0;
int			maintenance_work_mem = 65536;

/*
 * 决定共享内存结构大小的主要因素。
 *
 * MaxBackends 由 PostmasterMain 计算得到。
 */
int			NBuffers = 1000;
int			MaxConnections = 90;
int			MaxBackends = 0;

int			VacuumCostPageHit = 1;	/* vacuum 的 GUC 参数 */
int			VacuumCostPageMiss = 2;
int			VacuumCostPageDirty = 20;
int			VacuumCostLimit = 200;
double		VacuumCostDelay = 0;

int64		VacuumPageHit = 0;
int64		VacuumPageMiss = 0;
int64		VacuumPageDirty = 0;

int			VacuumCostBalance = 0;	/* vacuum 的工作状态 */
bool		VacuumCostActive = false;
