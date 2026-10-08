/* Copyright (c) 2013-2023 Jeffrey Pfau
*  Copyright (c) 2022 Felix Jones
*
* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <mgba/core/cheats.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/debugger/debugger.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#if defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
#include <mgba/internal/gba/sio/mobile.h>
#endif
#endif
#ifdef M_CORE_GB
#include <mgba/internal/sm83/sm83.h>
#endif
#ifdef ENABLE_SCRIPTING
#include <mgba/script.h>
#include <mgba/core/scripting.h>
#endif

#include <mgba/feature/commandline.h>
#include <mgba-util/vector.h>
#include <mgba-util/vfs.h>

#include <errno.h>
#include <signal.h>

#define HEADLESS_OPTIONS "S:R:"
static const char* const headlessUsage =
	"Additional options:\n"
	"  -S SWI           Run until specified SWI call before exiting\n"
	"  -R REGISTER      General purpose register to return as exit code\n"
#if defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
	"  --mobile-config FILE\n"
	"                   Load a 512-byte Mobile Adapter configuration\n"
#endif
#ifdef ENABLE_SCRIPTING
	"  --script FILE    Run a script on start. Can be passed multiple times\n"
#endif
	;

struct HeadlessOpts {
	int exitSwiImmediate;
	char* returnCodeRegister;
#if defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
	char* mobileConfig;
#endif
	struct StringList scripts;
};

static void _headlessShutdown(int signal);
static bool _parseHeadlessOpts(struct mSubParser* parser, int option, const char* arg);
static bool _parseLongHeadlessOpts(struct mSubParser* parser, const char* option, const char* arg);
static bool _parseSwi(const char* regStr, int* oSwi);

static bool _headlessCheckResiger(void);

static struct mCore* core;

static bool _dispatchExiting = false;
static int _exitCode = 0;
static struct mStandardLogger _logger;

#if defined(M_CORE_GBA) && defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
static struct GBASIOMobileAdapter _mobileAdapter;

static void _headlessInitDefaultMobileConfig(uint8_t* config);
static bool _headlessLoadMobileConfig(const char* path, uint8_t* config);
#endif

static void _headlessCallback(void* context);
#ifdef M_CORE_GBA
static void _headlessSwi16(struct ARMCore* cpu, int immediate);
static void _headlessSwi32(struct ARMCore* cpu, int immediate);

static int _exitSwiImmediate;
static char* _returnCodeRegister;

void (*_armSwi16)(struct ARMCore* cpu, int immediate);
void (*_armSwi32)(struct ARMCore* cpu, int immediate);
#endif

int main(int argc, char * argv[]) {
	signal(SIGINT, _headlessShutdown);

	bool cleanExit = false;
	int uncleanExit = 1;
	size_t i;

	struct HeadlessOpts headlessOpts = { 3, NULL };
	StringListInit(&headlessOpts.scripts, 0);
	struct mSubParser subparser = {
		.usage = headlessUsage,
		.parse = _parseHeadlessOpts,
		.parseLong = _parseLongHeadlessOpts,
		.extraOptions = HEADLESS_OPTIONS,
		.longOptions = (struct mOption[]) {
			{
				.name = "script",
				.arg = true,
			},
#if defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
			{
				.name = "mobile-config",
				.arg = true,
			},
#endif
			{0}
		},
		.opts = &headlessOpts
	};

	struct mArguments args;
	bool parsed = mArgumentsParse(&args, argc, argv, &subparser, 1);
	if (!args.fname) {
		parsed = false;
	}
	if (!parsed || args.showHelp) {
		usage(argv[0], NULL, NULL, &subparser, 1);
		uncleanExit = !parsed;
		goto argsExit;
	}
	if (args.showVersion) {
		version(argv[0]);
		uncleanExit = 0;
		goto argsExit;
	}
	core = mCoreFind(args.fname);
	if (!core) {
		goto argsExit;
	}
	core->init(core);
	mCoreInitConfig(core, "headless");
	mArgumentsApply(&args, NULL, 0, &core->config);

	mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "remove");
	mCoreConfigSetDefaultIntValue(&core->config, "logToStdout", true);
	mCoreLoadConfig(core);

	mStandardLoggerInit(&_logger);
	mStandardLoggerConfig(&_logger, &core->config);
	mLogSetDefaultLogger(&_logger.d);

	struct mCoreCallbacks callbacks = {0};

	_returnCodeRegister = headlessOpts.returnCodeRegister;
	if (!_headlessCheckResiger()) {
		goto loadError;
	}

	switch (core->platform(core)) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		((struct GBA*) core->board)->hardCrash = false;
		_exitSwiImmediate = headlessOpts.exitSwiImmediate;

#if defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
		GBASIOMobileAdapterCreate(&_mobileAdapter);
		const char* mobileConfig = headlessOpts.mobileConfig;
		if (!mobileConfig) {
			mobileConfig = getenv("MGBA_MOBILE_CONFIG");
		}
		if (mobileConfig && mobileConfig[0]) {
			if (!_headlessLoadMobileConfig(mobileConfig, _mobileAdapter.m.config)) {
				mLOG(STATUS, ERROR, "Could not load Mobile Adapter configuration: %s", mobileConfig);
				goto loadError;
			}
		} else {
			_headlessInitDefaultMobileConfig(_mobileAdapter.m.config);
		}
		core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &_mobileAdapter.d);
#endif

		if (_exitSwiImmediate == 3) {
			// Hook into SWI 3 (shutdown)
			callbacks.shutdown = _headlessCallback;
			core->addCoreCallbacks(core, &callbacks);
		} else {
			// Custom SWI hooks
			_armSwi16 = ((struct GBA*) core->board)->cpu->irqh.swi16;
			((struct GBA*) core->board)->cpu->irqh.swi16 = _headlessSwi16;
			_armSwi32 = ((struct GBA*) core->board)->cpu->irqh.swi32;
			((struct GBA*) core->board)->cpu->irqh.swi32 = _headlessSwi32;
		}
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		callbacks.shutdown = _headlessCallback;
		core->addCoreCallbacks(core, &callbacks);
		break;
#endif
	default:
		goto loadError;
	}

	if (!mCoreLoadFile(core, args.fname)) {
		goto loadError;
	}

#ifdef ENABLE_DEBUGGERS
	struct mDebugger debugger;
	mDebuggerInit(&debugger);
	bool hasDebugger = mArgumentsApplyDebugger(&args, core, &debugger);

	if (hasDebugger) {
		mDebuggerAttach(&debugger, core);
		mDebuggerEnter(&debugger, DEBUGGER_ENTER_MANUAL, NULL);
	} else {
		mDebuggerDeinit(&debugger);
	}
#endif

	core->reset(core);

	mArgumentsApplyFileLoads(&args, core);

	struct VFile* savestate = NULL;
	if (args.savestate) {
		savestate = VFileOpen(args.savestate, O_RDONLY);
	}
	if (savestate) {
		mCoreLoadStateNamed(core, savestate, 0);
		savestate->close(savestate);
	}

#ifdef ENABLE_SCRIPTING
	struct mScriptContext scriptContext;

	if (StringListSize(&headlessOpts.scripts)) {
		mScriptContextInit(&scriptContext);
		mScriptContextAttachStdlib(&scriptContext);
		mScriptContextAttachImage(&scriptContext);
		mScriptContextAttachLogger(&scriptContext, NULL);
		mScriptContextAttachSocket(&scriptContext);
#ifdef USE_JSON_C
		mScriptContextAttachStorage(&scriptContext);
#endif
		mScriptContextRegisterEngines(&scriptContext);

		mScriptContextAttachCore(&scriptContext, core);

		for (i = 0; i < StringListSize(&headlessOpts.scripts); ++i) {
			if (!mScriptContextLoadFile(&scriptContext, *StringListGetPointer(&headlessOpts.scripts, i))) {
				mLOG(STATUS, ERROR, "Failed to load script \"%s\"", *StringListGetPointer(&headlessOpts.scripts, i));
				goto scriptsError;
			}
		}
	}
#endif

#ifdef ENABLE_DEBUGGERS
	if (hasDebugger) {
		do {
			mDebuggerRun(&debugger);
		} while (!_dispatchExiting && debugger.state != DEBUGGER_SHUTDOWN);
	} else
#endif
	do {
		core->runLoop(core);
	} while (!_dispatchExiting);
	cleanExit = true;

scriptsError:
	core->unloadROM(core);

#ifdef ENABLE_SCRIPTING
	if (StringListSize(&headlessOpts.scripts)) {
		mScriptContextDeinit(&scriptContext);
	}
#endif

#ifdef ENABLE_DEBUGGERS
	if (hasDebugger) {
		core->detachDebugger(core);
		mDebuggerDeinit(&debugger);
	}
#endif

loadError:
	mStandardLoggerDeinit(&_logger);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	if (_returnCodeRegister) {
		free(_returnCodeRegister);
	}
#if defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
	if (headlessOpts.mobileConfig) {
		free(headlessOpts.mobileConfig);
	}
#endif

argsExit:
	for (i = 0; i < StringListSize(&headlessOpts.scripts); ++i) {
		free(*StringListGetPointer(&headlessOpts.scripts, i));
	}
	StringListDeinit(&headlessOpts.scripts);
	mArgumentsDeinit(&args);

	return cleanExit ? _exitCode : uncleanExit;
}

static void _headlessShutdown(int signal) {
	UNUSED(signal);
	_dispatchExiting = true;
}

static bool _headlessCheckResiger(void) {
	if (!_returnCodeRegister) {
		return true;
	}

	const struct mCoreRegisterInfo* registers;
	const struct mCoreRegisterInfo* reg = NULL;
	size_t regCount = core->listRegisters(core, &registers);
	size_t i;
	for (i = 0; i < regCount; ++i) {
		if (strcasecmp(_returnCodeRegister, registers[i].name) == 0) {
			reg = &registers[i];
			break;
		}
		if (registers[i].aliases) {
			size_t j;
			for (j = 0; registers[i].aliases[j]; ++j) {
				if (strcasecmp(_returnCodeRegister, registers[i].aliases[j]) == 0) {
					reg = &registers[i];
					break;
				}
			}
			if (reg) {
				break;
			}
		}
	}
	if (!reg) {
		return false;
	}

	if (reg->width > 4) {
		return false;
	}

	if (reg->type != mCORE_REGISTER_GPR) {
		return false;
	}

	if (reg->mask != 0xFFFFFFFFU >> (4 - reg->width) * 8) {
		return false;
	}

	return true;
}

static void _headlessCallback(void* context) {
	UNUSED(context);
	if (_returnCodeRegister) {
		core->readRegister(core, _returnCodeRegister, &_exitCode);
	}
	_dispatchExiting = true;
}

#ifdef M_CORE_GBA
static void _headlessSwi16(struct ARMCore* cpu, int immediate) {
	if (immediate == _exitSwiImmediate) {
		if (_returnCodeRegister) {
			core->readRegister(core, _returnCodeRegister, &_exitCode);
		}
		_dispatchExiting = true;
		return;
	}
	_armSwi16(cpu, immediate);
}

static void _headlessSwi32(struct ARMCore* cpu, int immediate) {
	if (immediate == _exitSwiImmediate) {
		if (_returnCodeRegister) {
			core->readRegister(core, _returnCodeRegister, &_exitCode);
		}
		_dispatchExiting = true;
		return;
	}
	_armSwi32(cpu, immediate);
}
#endif

static bool _parseHeadlessOpts(struct mSubParser* parser, int option, const char* arg) {
	struct HeadlessOpts* opts = parser->opts;
	errno = 0;
	switch (option) {
	case 'S':
		return _parseSwi(arg, &opts->exitSwiImmediate);
	case 'R':
		opts->returnCodeRegister = strdup(arg);
		return true;
	default:
		return false;
	}
}

static bool _parseLongHeadlessOpts(struct mSubParser* parser, const char* option, const char* arg) {
	struct HeadlessOpts* opts = parser->opts;
	if (strcmp(option, "script") == 0) {
		*StringListAppend(&opts->scripts) = strdup(arg);
		return true;
	}
#if defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
	if (strcmp(option, "mobile-config") == 0) {
		opts->mobileConfig = strdup(arg);
		return true;
	}
#endif
	return false;
}

static bool _parseSwi(const char* swiStr, int* oSwi) {
	char* parseEnd;
	long swi = strtol(swiStr, &parseEnd, 0);
	if (errno || swi > UINT8_MAX || *parseEnd) {
		return false;
	}
	*oSwi = swi;
	return true;
}

#if defined(M_CORE_GBA) && defined(USE_LIBMOBILE) && defined(MOBILE_TEST_RUNNER)
static void _headlessInitDefaultMobileConfig(uint8_t* config) {
	memset(config, 0, MOBILE_CONFIG_SIZE);
	config[0] = 'M';
	config[1] = 'A';
	config[2] = 0x81;
	memcpy(&config[12], "test-user", sizeof("test-user"));
	memcpy(&config[44], "test@example.com", sizeof("test@example.com"));
	config[118] = 0xA9; // #9
	config[119] = 0x67; // 67
	config[120] = 0x7F; // 7 + terminator
	memcpy(&config[126], "mGBA test", sizeof("mGBA test"));

	uint16_t checksum = 0;
	for (size_t i = 0; i < 0xBE; ++i) {
		checksum += config[i];
	}
	config[0xBE] = checksum >> 8;
	config[0xBF] = checksum;
}

static bool _headlessLoadMobileConfig(const char* path, uint8_t* config) {
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		return false;
	}
	if (vf->size(vf) != MOBILE_CONFIG_SIZE) {
		vf->close(vf);
		return false;
	}
	ssize_t size = vf->read(vf, config, MOBILE_CONFIG_SIZE);
	vf->close(vf);
	return size == MOBILE_CONFIG_SIZE;
}
#endif
