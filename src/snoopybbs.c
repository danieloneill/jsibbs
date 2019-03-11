#include "jsi.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <pty.h>
#include <sys/wait.h>

#include <sys/prctl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <termios.h>
#include <sys/ioctl.h>

#include <sys/select.h>
#include <sys/stat.h>

#include <sys/socket.h>

#define MAX_ARGS 32

typedef struct { /* Interp wide data. */
#ifdef JSI_HAS_SIG
	jsi_Sig sig;
#endif
	Jsi_Interp *interp;
	Jsi_Hash *wsTable;
	int wIdx;
} DoorObjInterpData;

typedef struct {
#ifdef JSI_HAS_SIG
	jsi_Sig sig;
#endif
	DoorObjInterpData *interpData; // necessary?
	Jsi_Interp *interp;

	int objid;
	Jsi_Obj *fobj;

	Jsi_Event *event;

	Jsi_Value *onRecv;
	Jsi_Value *onClose;
	Jsi_Value *execDir;

	char exepath[64];
	char *nargv[MAX_ARGS];
	uint8_t is_alive; // if 1, PTY is open, FDs are receptive. Otherwise 0.
	uint8_t is_running; // if 1, process is running. Otherwise 0.
	int fd_master;
	int fd_slave;
	char ptyname[64];
	pid_t childpid;
} DoorObj;

static Jsi_RC doorUpdate(Jsi_Interp *interp, void *data);

static Jsi_CmdProcDecl(DoorOpenPtyCmd)
{
	DoorObj *cmdPtr = (DoorObj *)Jsi_UserObjGetData(interp, _this, funcPtr);
	if( cmdPtr->is_alive == 1 )
	{
		Jsi_ValueMakeStringDup(interp, ret, cmdPtr->ptyname);
		return JSI_OK;
	}

	if( 0 != openpty( &cmdPtr->fd_master, &cmdPtr->fd_slave, NULL, NULL, NULL ) )
		return Jsi_LogError("Door.open() failed at openpty.");

	if( 0 != ttyname_r( cmdPtr->fd_slave, cmdPtr->ptyname, sizeof(cmdPtr->ptyname) ) )
	{
		close( cmdPtr->fd_master );
		close( cmdPtr->fd_slave );
		return Jsi_LogError("Door.open() failed at ttyname_r.");
	}

        if( 0 != fchmod( cmdPtr->fd_slave, S_IRWXU|S_IRWXG|S_IRWXO ) )
	{
		close( cmdPtr->fd_master );
		close( cmdPtr->fd_slave );
		return Jsi_LogError("Door.open() failed at fchmod on PTY device.");
	}

	cmdPtr->is_alive = 1;

	Jsi_ValueMakeStringDup(interp, ret, cmdPtr->ptyname);
	return JSI_OK;
}

static Jsi_CmdProcDecl(DoorRunCmd)
{
	DoorObj *cmdPtr = (DoorObj *)Jsi_UserObjGetData(interp, _this, funcPtr);
	if( cmdPtr->is_alive == 0 )
		return Jsi_LogError("Door.run() called without an open PTY. (Did you forget to call openpty()?)");

	if( cmdPtr->is_running == 1 )
	{
		Jsi_ValueMakeBool(interp, ret, 1);
		return JSI_OK;
	}

	// Optional parameters:
	int argc = Jsi_ValueGetLength(interp, args);
	memset( cmdPtr->nargv, 0, (sizeof(char*) * MAX_ARGS) );
	cmdPtr->nargv[0] = cmdPtr->exepath;
	if( argc > 0 )
	{
		// walk through the provided array:
		Jsi_Value *argarr = Jsi_ValueArrayIndex(interp, args, 0);
		int argcnt = Jsi_ValueGetLength(interp, argarr);
	
		int z = 1;
		for( int i=0; i < argcnt && i < (MAX_ARGS - 2); i++ )
		{
			Jsi_Value *v = Jsi_ValueArrayIndex(interp, argarr, i);
			if (!v) continue;
			const char *cp = Jsi_ValueString(interp, v, 0);
			if (!cp) continue;
			cmdPtr->nargv[z++] = strdup(cp);
		}
	}

	cmdPtr->childpid = fork();
	if( 0 == cmdPtr->childpid )
	{
		// Child.
		prctl(PR_SET_PDEATHSIG, SIGTERM);

		char *newdir = NULL;
		if( cmdPtr->execDir && Jsi_ValueIsString(interp, cmdPtr->execDir) )
		{
			newdir = (char*)Jsi_ValueString(interp, cmdPtr->execDir, NULL);
			chdir(newdir);
			fprintf(stderr, "(child) I am running '%s' from directory '%s'. My first 5 args are: %s %s %s %s %s\n", cmdPtr->exepath, get_current_dir_name(),
					cmdPtr->nargv[0],
					cmdPtr->nargv[1],
					cmdPtr->nargv[2],
					cmdPtr->nargv[3],
					cmdPtr->nargv[4]);
		}
 
		execvp(cmdPtr->exepath, cmdPtr->nargv);
		Jsi_LogError("Door.open() failed at execvp.");
		exit(-1);
	}

	cmdPtr->is_running = 1;

	Jsi_ValueMakeBool(interp, ret, 1);
	return JSI_OK;
}

void doorShutdown( Jsi_Interp *interp, DoorObj *cmdPtr )
{
	uint8_t firstClose = ( cmdPtr->is_alive ) ? 1 : 0;

	if( cmdPtr->is_running == 1 )
	{
		cmdPtr->is_running = 0;

		kill( cmdPtr->childpid, SIGTERM );
		waitpid( cmdPtr->childpid, NULL, 0 );
	}

	if( cmdPtr->is_alive == 1 )
	{
		cmdPtr->is_alive = 0;

		close( cmdPtr->fd_master );
		close( cmdPtr->fd_slave );
	}

	for( int i=1; i < MAX_ARGS; i++ )
	{
		if( cmdPtr->nargv[i] )
		{
			free( cmdPtr->nargv[i] );
			cmdPtr->nargv[i] = NULL;
		}
	}

	if( firstClose && cmdPtr->onClose )
	{
		/* Pass 1 args: doorobj. */
		Jsi_Value *vpargs, *vargs[2], *ret = Jsi_ValueNew1(interp);
		int n = 0;
		vargs[n++] = Jsi_ValueNewObj(interp, cmdPtr->fobj);
		vpargs = Jsi_ValueMakeObject(interp, NULL, Jsi_ObjNewArray(interp, vargs, n, 0));
		Jsi_IncrRefCount(interp, vpargs);
		Jsi_ValueMakeUndef(interp, &ret);
		int src = Jsi_FunctionInvoke(interp, cmdPtr->onClose, vpargs, &ret, NULL);
		Jsi_DecrRefCount(interp, vpargs);
		Jsi_DecrRefCount(interp, ret);
		if (src != JSI_OK)
			Jsi_LogError("Failed to execute onRecv hook attached to Door instance.");
	}
}

static Jsi_CmdProcDecl(DoorCloseCmd)
{
	DoorObj *cmdPtr = (DoorObj *)Jsi_UserObjGetData(interp, _this, funcPtr);
	if( cmdPtr->is_alive == 0 && cmdPtr->is_running == 0 )
		return JSI_OK;

	doorShutdown( interp, cmdPtr );

	return JSI_OK;
}

/*
static Jsi_CmdProcDecl(DoorRecvCmd)
{
	DoorObj *cmdPtr = (DoorObj *)Jsi_UserObjGetData(interp, _this, funcPtr);
	if( cmdPtr->is_alive == 0 )
		return JSI_ERROR;

	return JSI_OK;
}
*/

static Jsi_CmdProcDecl(DoorSendCmd)
{
	DoorObj *cmdPtr = (DoorObj *)Jsi_UserObjGetData(interp, _this, funcPtr);
	if( cmdPtr->is_alive == 0 || cmdPtr->is_running == 0 )
		return Jsi_LogError("Door.send() attempted on a closed Door instance (did you forget to call openpty() or run()?).");

	int argc = Jsi_ValueGetLength(interp, args);
	if( argc < 1 )
		return Jsi_LogError("Door.send() requires a data parameter.");

	Jsi_Value *data = Jsi_ValueArrayIndex(interp, args, 0);
	if( data == NULL || !Jsi_ValueIsString(interp, data) )
		return Jsi_LogError("Door.send() requires data parameter to be a string.");

	int blen;
	const char *buf = Jsi_ValueString(interp, data, &blen);
	if( blen < 0 || !buf )
		return Jsi_LogError("Door.send() requires data parameter to be a non-empty string.");

	if( blen == 0 )
	{
		Jsi_ValueMakeNumber(interp, ret, 0);
		return JSI_OK;
	}

	int written = write( cmdPtr->fd_master, buf, blen );
	Jsi_ValueMakeNumber(interp, ret, written);

	return JSI_OK;
}

static Jsi_CmdProcDecl(DoorUpdateCmd)
{
	DoorObj *cmdPtr = (DoorObj *)Jsi_UserObjGetData(interp, _this, funcPtr);
	if( cmdPtr->is_alive == 0 || cmdPtr->is_running == 0 )
		return JSI_OK;

	return doorUpdate(interp, cmdPtr);
}

static Jsi_OptionSpec DoorOptions[] = {
    JSI_OPT(FUNC,   DoorObj, onClose,    .help="Function to call when door closes", .flags=0, .custom=0, .data=(void*)"s:userobj"),
    JSI_OPT(FUNC,   DoorObj, onRecv,     .help="Function to call with recieved data", .flags=0, .custom=0, .data=(void*)"s:userobj, data:string"),
    JSI_OPT(STRING, DoorObj, execDir,    .help="Launch EXE from specified directory, or current directory if unspecified."),
    JSI_OPT_END(DoorObj, .help="Door options")
};

static Jsi_CmdSpec doorCmds[] = {
    { "openpty",    DoorOpenPtyCmd,  0,  0, "", .help="Open PTY and return the filesystem path", .retType=(uint)JSI_TT_STRING },
    { "run",        DoorRunCmd,      0,  1, "args:array=void", .help="Launch EXE in the background, optionally with parameters", .retType=(uint)JSI_TT_BOOLEAN },
    { "close",      DoorCloseCmd,    0,  0, "", .help="Kill and close active door and PTY", .retType=(uint)JSI_TT_VOID },
    //{ "recv",       DoorRecvCmd,     0,  0, "", .help="Recieve data from door PTY", .retType=(uint)JSI_TT_STRING },
    { "send",       DoorSendCmd,     1,  1, "data:string", .help="Send data to door PTY", .retType=(uint)JSI_TT_NUMBER },
    { "update",     DoorUpdateCmd,   0,  0, "", .help="Service events for just this door/PTY instance", .retType=(uint)JSI_TT_VOID },
    { NULL, 0,0,0,0, .help="Commands for managing Door object"  }
};

static Jsi_RC doorObjFree(Jsi_Interp *interp, void *data)
{
    DoorObj *cmdPtr = (DoorObj *)data;

    doorShutdown( interp, cmdPtr );

    Jsi_EventFree(cmdPtr->interp, cmdPtr->event);
    Jsi_OptionsFree(cmdPtr->interp, DoorOptions, cmdPtr, 0);
    cmdPtr->interp = NULL;
    
    // close FDs and crap.
    // not much to 'free' per se.

    Jsi_Free(cmdPtr);
    return JSI_OK;
}

static bool doorObjIsTrue(void *data)
{
    return 1;
   /* if (!fo->sockname) return 0;
    else return 1;*/
}

static bool doorObjEqual(void *data1, void *data2)
{
    return (data1 == data2);
}

static Jsi_UserObjReg doorobject = {
    "DoorObject",
    doorCmds,
    doorObjFree,
    doorObjIsTrue,
    doorObjEqual
};

static Jsi_RC doorUpdate(Jsi_Interp *interp, void *data)
{
	DoorObj *cmdPtr = (DoorObj *)data;

	if( cmdPtr->is_alive == 0 || cmdPtr->is_running == 0 )
		return JSI_OK;

	while(1)
	{
		// Poll here.
		struct timeval tv;
		fd_set rx, ex;
		int mx = cmdPtr->fd_master;
		FD_ZERO( &rx );
		FD_ZERO( &ex );
	
		FD_SET( cmdPtr->fd_master, &rx );
		FD_SET( cmdPtr->fd_master, &ex );
		FD_SET( cmdPtr->fd_slave, &rx );
		FD_SET( cmdPtr->fd_slave, &ex );
	
		if( cmdPtr->fd_slave > mx )
			mx = cmdPtr->fd_slave;
		tv.tv_sec = 0;
		tv.tv_usec = 1;
	
		int sret = select( mx+1, &rx, NULL, &ex, &tv );
		if( sret < 0 )
		{
			return Jsi_LogError("Failed to poll Door instance.");
		}
	
		// check if BRE exited:
		int wstatus;
		pid_t wp = waitpid( cmdPtr->childpid, &wstatus, WNOHANG );
		if( wp > 0 )
		{
			doorShutdown( interp, cmdPtr );
			return JSI_OK;
		}
	
		if( sret == 0 )
			return JSI_OK;
	
		if( FD_ISSET( cmdPtr->fd_master, &ex )
		 || FD_ISSET( cmdPtr->fd_slave, &ex ) )
		{
			doorShutdown( interp, cmdPtr );
			return JSI_OK;
		}
	
		if( FD_ISSET( cmdPtr->fd_master, &rx ) )
		{
			char buf[4096];
			ssize_t towrite = read( cmdPtr->fd_master, buf, sizeof(buf) );
			if( towrite == 0 )
				return JSI_OK;
	
			if( towrite < 0 )
			{
				doorShutdown( interp, cmdPtr );
				return JSI_OK;
			}
	
			if( cmdPtr->onRecv )
			{
				/* Pass 2 args: doorobj, data. */
				Jsi_Value *vpargs, *vargs[3], *ret = Jsi_ValueNew1(interp);
				int n = 0;
				vargs[n++] = Jsi_ValueNewObj(interp, cmdPtr->fobj);
				vargs[n++]  = Jsi_ValueNewBlob(interp, (uchar*)buf, towrite);
				vpargs = Jsi_ValueMakeObject(interp, NULL, Jsi_ObjNewArray(interp, vargs, n, 0));
				Jsi_IncrRefCount(interp, vpargs);
				Jsi_ValueMakeUndef(interp, &ret);
				int src = Jsi_FunctionInvoke(interp, cmdPtr->onRecv, vpargs, &ret, NULL);
				Jsi_DecrRefCount(interp, vpargs);
				Jsi_DecrRefCount(interp, ret);
				if (src != JSI_OK)
					return Jsi_LogError("Failed to execute onRecv hook attached to Door instance.");
			}
		}
	}

	return JSI_OK;
}

static Jsi_CmdProcDecl(DoorConstructor)
{
	DoorObj *cmdPtr = (DoorObj *)Jsi_Calloc(1, sizeof(DoorObj));
	int argc = Jsi_ValueGetLength(interp, args);
	if( argc < 2 )
		goto bail;

	Jsi_Value *argPath = Jsi_ValueArrayIndex(interp, args, 0);
	Jsi_Value *argOptions = Jsi_ValueArrayIndex(interp, args, 1);

	if( argPath == NULL || !Jsi_ValueIsString(interp, argPath) )
		goto bail;

	const char *constrPath = Jsi_ValueString(interp, argPath, NULL);
	strncpy( cmdPtr->exepath, constrPath, sizeof(cmdPtr->exepath) );

	cmdPtr->is_alive = 0;
	cmdPtr->is_running = 0;
	cmdPtr->interp = interp;
	cmdPtr->event = Jsi_EventNew(interp, doorUpdate, cmdPtr);
	Jsi_Obj *o = Jsi_ObjNew(interp);
	Jsi_PrototypeObjSet(interp, "DoorObject", o);
	Jsi_ValueMakeObject(interp, ret, o);

	if ((argOptions != NULL && !Jsi_ValueIsNull(interp,argOptions))
 	       && Jsi_OptionsProcess(interp, DoorOptions, cmdPtr, argOptions, 0) < 0)
		goto bail;

	cmdPtr->fobj = Jsi_ValueGetObj(interp, *ret);
	if ((cmdPtr->objid = Jsi_UserObjNew(interp, &doorobject, cmdPtr->fobj, cmdPtr))<0)
		goto bail;

	// Make all the things!

	return JSI_OK;

bail:
	doorObjFree(interp, cmdPtr);
	Jsi_ValueMakeUndef(interp, ret);
	return JSI_ERROR;
}

static Jsi_CmdSpec doorConstructor[] = {
    { "Door",       DoorConstructor, 2,  2, "path:string, options:object", .help="Launch door executable at 'path'",
            .retType=(uint)JSI_TT_USEROBJ, .flags=JSI_CMD_IS_CONSTRUCTOR, .info=JSI_INFO("\
Create a Door object according to provided parameters."), .opts=DoorOptions },
    { NULL, 0,0,0,0, .help="The constructor for creating a Door object."  }
};


Jsi_InitProc Jsi_InitCmds;

Jsi_RC Jsi_InitCmds(Jsi_Interp *interp, int release) {
    Jsi_CommandCreateSpecs(interp, "Door", doorConstructor, NULL, 0);

    Jsi_Hash *wsys;
    if (!(wsys = Jsi_UserObjRegister(interp, &doorobject))) {
        Jsi_LogBug("Can not init doorobj");
        return JSI_ERROR;
    }
    if (Jsi_PkgProvide(interp, "Door", 2, Jsi_InitCmds) != JSI_OK)
        return JSI_ERROR;

    if (!Jsi_CommandCreateSpecs(interp, doorobject.name, doorCmds, wsys, JSI_CMDSPEC_ISOBJ))
        return JSI_ERROR;

    return JSI_OK;
}

#ifndef JSI_LITE_ONLY
int jsi_main(int argc, char **argv)
{
     // A replacement for shebang "#!/usr/bin/env".
    Jsi_DString sStr = {};
    FILE *fp = NULL;
    if (argc >= 3 && Jsi_Strchr(argv[1], ' ') && Jsi_Strstr(argv[1], "%s")) {
        Jsi_DString tStr = {};
        int i;
        Jsi_DSAppend(&tStr, argv[0], " ", NULL);
        Jsi_DSPrintf(&tStr, argv[1], argv[2]);
        for (i=3; i<argc; i++)
            Jsi_DSAppend(&tStr, " ", argv[i], NULL);
        Jsi_SplitStr(Jsi_DSValue(&tStr), &argc, &argv, NULL, &sStr);
        Jsi_DSFree(&tStr);
    }
    // Perform shebang extraction.
    else if (argc == 3 && !Jsi_Strcmp(argv[1], "-!") && (fp=fopen(argv[2],"r+"))) {
        char ibuf[1024], *icp = fgets(ibuf, sizeof(ibuf), fp);
        fclose(fp);
        if (icp && icp[0] == '#' && icp[1] == '!') {
            Jsi_DString tStr = {};
            icp += 2;
            if (strstr(icp, "%s")) {
                icp = Jsi_DSPrintf(&tStr, icp, argv[2]);
            } else {
                int len = strlen(icp);
                if (len>0 && icp[len-1]=='\n') icp[len-1] = 0;
                icp = Jsi_DSAppend(&tStr, icp, " ", argv[2], NULL);
            }
            int rc = system(icp);
            Jsi_DSFree(&tStr);
            return rc;
        }
    }
    Jsi_InterpOpts opts = {.argc=argc, .argv=argv};
    Jsi_Interp *interp = Jsi_InterpNew(&opts);

    if (Jsi_InitCmds(interp, 0) != JSI_OK)
        exit(1);

    //opts.interp = interp;
    Jsi_Main(&opts);
    if (!interp) return opts.exitCode;
    Jsi_InterpDelete(interp);
    Jsi_DSFree(&sStr);
    exit(0);
}

#if JSI__MAIN
int main(int argc, char **argv) { return jsi_main(argc, argv); }
#endif
#endif
