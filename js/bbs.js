#!/home/doneill/code/jsi/jsish

require('cprint');

var db = new Sqlite('snoopybbs.db');

db.eval("CREATE TABLE IF NOT EXISTS users( id INTEGER PRIMARY KEY NOT NULL, username VARCHAR(64) NOT NULL, password VARCHAR(64) NOT NULL, realname VARCHAR(64) NOT NULL, location VARCHAR(64) )");

// Used for tracking 'where' the client is on the BBS:
var client = {};

function pressAKey( id:number )
{
	net.send( id, cprint(" [!red]([red]~ [!white]P[white]ress a [!white]K[white]ey [red]~[!red]) ") );
}

function filler( count:number )
{
	var x = ' ';
	return x.repeat( count );
}

// Drop file writers:
function writeDoorfileSR( id:number, path:string )
{
	var contents = [ ""+client[id]['info']['username'],
			"1",
			"1",
			"24",
			"115200",
			"1",
			"120",
			""+client[id]['info']['username']
	];
	File.write(path, ""+contents.join("\r\n")+"\r\n");
}

function writeChain( id:number, path:string )
{
	var chain = [	""+client[id]['info']['id'],		// id
			""+client[id]['info']['username'],	// alias
			""+client[id]['info']['realname'],	// realname
			'',					// callsign
			'0',	// age
			'M', // sex
			'', // gold?
			'01/01/99', // last login date
			'80', // width
			'24', //height
			'50', // sec lev
			'0', // co-sysop?
			'0', // sysop?
			'1', // ANSI?
			'1', // 1 = remote, 0 = local
			'2225.78', // secs left
			"D:\\DOS", // just a path
			"D:\\DOS", // just a path
			"CHAIN.LOG", // logfile
			"115200", // baud rate
			"1", // com port
			"Snoopy BBS", // BBS NAME
			"mortanian", // sysop's name
			'83680', // more 'time logged on' crap
			'0', // secs online
			'0', // kb uploaded
			'0', // num uploads
			'0', // kb downloaded
			'0', // num downloads
			'8N1', // com settings
			'115200', // com rate
			'0' // wwiv node number
	];
		
	File.write( path, ""+chain.join("\r\n")+"\r\n" );
}

function writeDoorSys( id:number, path:string )
{
	var contents = [
		"COM1:", // port (COM0: = local)
		"115200", // baud rate
		"8", // parity
		"1", // node #
		"115200", // actual rate
		"Y", // screen display
		"Y", // printer toggle
		"Y", // page bell
		"Y", // caller alarm
		client[id]['info']['realname'], // name
		client[id]['info']['location'], // location
		"", // home phone
		"", // work phone
		"", // password
		"50", // security level
		"1", // login count
		"01/01/01", // last date called
		""+(60*60*4), // seconds remaining on this call
		""+(60*4), // mins remaining on this call
		"GR", // graphics mode (GR, NG, 7E)
		"24", // rows
		"Y", // user mode (Y=Expert, N=Novice)
		"", // subscribed forums
		"", // forums to exit door to
		"", // user expiration date
		""+client[id]['info']['id'], // userid
		"Y", // default transfer protocol
		"0", // total uploads
		"0", // total downloads
		"0", // KB download daily total
		"0" // KB download daily max
	];
	File.write( path, ""+contents.join("\r\n")+"\r\n" );
}

// Utility function to send *.ANS files to the client:
function sendFile( id:number, path:string )
{
	var fc = File.read(path);

	// Parse for <% and %> tags.
	fc = fc.replace( /<%(\w+?):([0-9]+)%>/g, function(matches, p1, p2, p3, p4) {
		var wid = parseInt(p2);

		if( p1 == 'CURTIME' )
		{
			var ctime = strftime(null, '%I:%m%P');
			return ctime;
		}
		else if( p1 == 'REALNAME' )
		{
			var rname = ""+client[id]['info']['realname'];
			rname += filler( wid - rname.length );
			return rname;
		}
		else if( p1 == 'LOCATION' )
		{
			var rname = ""+client[id]['info']['location'];
			rname += filler( wid - rname.length );
			return rname;
		}

		return match;
	});
	net.send( id, fc );
}

var handlerAccountInfo = {
	onEnter: function( id:number )
	{
		sendFile( id, 'ansi/account.ans' );
		sendFile( id, 'ansi/prompt.ans' );
	},

	onRecv: function( id:number, data:string )
	{
		if( data == 'r' || data == 'R' )
			setHandler( id, handlerMainMenu );
	},

	onExit: function( id:number )
	{
	}
};

var handlerBanner = {
	onEnter: function( id:number )
	{
		sendFile( id, 'ansi/banner.ans' );
		pressAKey( id );
	},

	onRecv: function( id:number, data:string )
	{
		setHandler( id, handlerLogin );
	},

	onExit: function( id:number )
	{
	}
};

function launchDoor( id:number, doordir:string, exename:string )
{
	var script = '/usr/bin/dosemu';
	var door = new Door(script, {
		'onRecv': function(obj, data)
		{
			net.send( id, data );
		},
		'onClose': function(obj)
		{
			//door.close();
			delete door;
			delete client[id]['door'];
			setHandler( id, handlerDoors );
		},
		'execDir': doordir
	});

	var path = door.openpty();
	if( !path )
	{
		delete door;
		return setHandler( id, handlerDoors );
	}

	File.write(doordir+'/dosemu.rc', "$_com1 = \""+path+"\"\n");

	puts( cprint( '[!yellow] +++ [yellow]Launching [!white]'+exename+'[yellow] from directory [white]'+doordir+' on PTY [!white]'+path) );
	if( !door.run( ['-f', 'dosemu.rc', '-dumb', '-O', exename] ) )
	{
		door.close();
		delete door;
		return setHandler( id, handlerDoors );
	}

	client[id]['door'] = door;
	setHandler( id, handlerDoor );
}

var handlerDoors = {
	onEnter: function( id:number )
	{
		sendFile( id, 'ansi/doors.ans' );
		sendFile( id, 'ansi/prompt.ans' );
	},

	onRecv: function( id:number, data:string )
	{
		if( data == 'r' || data == 'R' )
			setHandler( id, handlerMainMenu );
		else if( data == '1' )
		{
			var doordir = '/home/doneill/dos/doors/lord';
			var exename = doordir+'/START1.BAT';
			writeDoorSys( id, doordir+'/node1/DOOR.SYS' );
			writeChain( id, doordir+'/node1/CHAIN.TXT' );
			launchDoor( id, doordir, exename );
		}
		else if( data == '2' )
		{
			var doordir = '/home/doneill/dos/doors/bre';
			var exename = doordir+'/BRE.BAT';
			writeDoorfileSR( id, doordir+'/DOORFILE.SR' );
			launchDoor( id, doordir, exename );
		}
	},

	onExit: function( id:number )
	{
	}
};

var handlerDoor = {
	onEnter: function( id:number )
	{
		net.send( id, cprint(" [!blue]([blue]~ [!white]L[white]oading... [blue]~[!blue]) ") );
	},

	onRecv: function( id:number, data:string )
	{
		if( client[id]['door'] )
			client[id]['door'].send( data );
	},

	onExit: function( id:number )
	{
		net.send( id, System.format("%c[2J%c[0;0H", 0x1B, 0x1B) ); // clear screen & home
	}
};

function resetInput( id:number )
{
	client[id]['inputBuffer'] = '';
}

function handleInput( id:number, data:string, masked:boolean=false )
{
	for( var x=0; x < data.length; x++ )
	{
		var c = data.charAt(x);
		var ch = data.charCodeAt(x);
		if( ch == 10 || ch == 13 )
		{
			var ret = client[id]['inputBuffer'];
			resetInput( id );
			return ret;
		}
		else if( ch == 8 || ch == 0x7F )
		{
			if( client[id]['inputBuffer'].length == 0 )
				return;
	
			client[id]['inputBuffer'] = client[id]['inputBuffer'].substr(0, client[id]['inputBuffer'].length - 1);
			net.send( id, System.format("%c[1D %c[1D", 0x1B, 0x1B) );
			return true;
		}
		else if( ch >= 32 && ch <= 126 )
		{
			client[id]['inputBuffer'] += c;
			if( masked )
				net.send( id, '*' );
			else
				net.send( id, c );
			return true;
		}
	}
	return false;
}

var handlerLogin = {
	onEnter: function( id:number )
	{
		client[id]['login'] = {
			'state': 'username',
			'username': '',
			'password': ''
		};
		net.send( id, cprint("\r                          \r\n\r\n[white]To create a new account, use the username [!white]new[white].\r\n\r\n [!red]U[red]sername[!white]:[white] ") );
		resetInput(id);
	},

	onRecv: function( id:number, data:string )
	{
		if( client[id]['login']['state'] == 'username' )
		{
			var ret = handleInput(id, data);
			if( typeof ret == 'string' && ret.length > 0 )
			{
				client[id]['login']['username'] = ret;
				client[id]['login']['state'] = 'password';
				net.send( id, cprint("\r\n [!red]P[red]assword[!white]:[white] ") );
			}
		}
		else if( client[id]['login']['state'] == 'password' )
		{
			var ret = handleInput(id, data, true);
			if( typeof ret == 'string' && ret.length > 0 )
			{
				client[id]['login']['password'] = ret;

				net.send( id, cprint("\r\n [!red]Checking...\r\n" ));
				var q = db.query("SELECT id, username, password, realname, location FROM users WHERE username=? AND password=?", {'values':[ client[id]['login']['username'].toLowerCase(), client[id]['login']['password'].toLowerCase() ], 'mode':'rows'} );
				if( q.length == 1 )
				{
					client[id]['info'] = q[0];
					net.send( id, cprint("\r\n [!blue]W[blue]elcome, [!white]"+q[0]['username']+"[blue]!\r\n\r\n") );
					setHandler( id, handlerMainMenu );
				}
				else {
					setTimeout( function() {
						net.send( id, cprint("\r\n [red]Failed to authenticate!\r\n\r\n") );

						handlerLogin['onEnter']( id );
					}, 3000 );
				}
			}
		}
	},

	onExit: function( id:number )
	{
	}
};

var handlerMainMenu = {
	onEnter: function( id:number )
	{
		net.send( id, System.format("%c[2J%c[0;0H", 0x1B, 0x1B) ); // clear screen & home
		sendFile( id, 'ansi/mainmenu.ans' );
		sendFile( id, 'ansi/prompt.ans' );
	},

	onRecv: function( id:number, data:string )
	{
		if( data == 'a' || data == 'A' )
			setHandler( id, handlerAccountInfo );
		else if( data == 'd' || data == 'D' )
			setHandler( id, handlerDoors );
		else if( data == 's' || data == 'S' )
			setHandler( id, handlerServerInfo );
		else if( data == 'l' || data == 'L' )
		{
			console.log("Closing...");
			net.close(id);
		}
	},

	onExit: function( id:number )
	{
	}
};

var handlerServerInfo = {
	onEnter: function( id:number )
	{
		sendFile( id, 'ansi/serverinfo.ans' );
		net.send( id, "\r\n\r\n" );
		pressAKey( id );
	},

	onRecv: function( id:number, data:string )
	{
		net.send( id, cprint("\r                          \r\n\r\n") );
		setHandler( id, handlerMainMenu );
	},

	onExit: function( id:number )
	{
	}
};

function setHandler( id:number, handler:object )
{
	if( client[id]['handler'] && client[id]['handler']['onExit'] )
		client[id]['handler']['onExit']( id );

	client[id]['handler'] = handler;
	if( handler['onEnter'] )
		handler['onEnter']( id );
}

// Handlers for Open, Recv, and Close socket events:
function handleOpen( s:userobj, info:object )
{
	console.log("Open from: "+JSON.stringify(s));
	console.log("Info: "+JSON.stringify(info));
	var id = info['id'];
	client[ id ] = {
		'sendbuf': System.format("%c%c%c", 0xFF, 251, 1),
		'handler':false
	};

	setTimeout( function() {
		setHandler( id, handlerBanner );
	}, 1000);

	return true;
}

function handleRecv( s:userobj, id:number, data:string )
{
	/*
	console.log("Recv from: "+JSON.stringify(s));
	console.log("Recv ID: "+id);
	console.log("Recv data: ");
	*/

	var recvbuf = '';
	var sendbuf = client[id]['sendbuf'];
	client[id]['sendbuf'] = '';
	var inIAR = false;
	var inCMD = false;
	var cmd = 0;
	var opt = 0;
	var inWinSize = 0;
	for( var x=0; x < data.length; x++ )
	{
		var ch = data.charCodeAt(x);
		//printf("[%d]", ch);
		if( inWinSize > 0 )
		{
			inWinSize--;
			continue;
		}
		else
		if( ch == 255 )
			inIAR = true;
		else if( inIAR )
		{
			inIAR = false;
			if( ch < 240 || ch > 254 )
				continue;

			inCMD = true;
			cmd = ch;
		}
		else if( inCMD )
		{
			inCMD = false;
			opt = ch;

			// respond to commands, but ... let's do it all at once with a sendbuf.
			if( cmd == 253 ) { // do
				if( opt == 3 ) // suppress 'go ahead'
					sendbuf += System.format("%c%c%c", 0xFF, 251, 3); // will suppress go ahead
			}
			else if( cmd == 250 ) { // subnegotiation
				if( opt == 31 )
				{
					// getting remote window size...
					inWinSize = 6;
				}
			}
			else if( cmd == 251 ) { // will
				if( opt == 24 ) // terminal type
					sendbuf += System.format("%c%c%c", 0xFF, 253, opt); // do terminal type
				else if( opt == 31 ) // window size
					//sendbuf += System.format("%c%c%c", 0xFF, 254, opt); // do NOT window size
					sendbuf += System.format("%c%c%c", 0xFF, 253, opt); // do window size
				else if( opt == 32 ) // terminal speed
					sendbuf += System.format("%c%c%c", 0xFF, 254, opt); // do NOT terminal speed
				else if( opt == 33 ) // remote flow control
					sendbuf += System.format("%c%c%c", 0xFF, 254, opt); // do NOT flow control
				else if( opt == 34 ) // linemode
					sendbuf += System.format("%c%c%c", 0xFF, 254, opt); // do NOT linemode
				else if( opt == 35 ) // ??
					sendbuf += System.format("%c%c%c", 0xFF, 254, opt); // do NOT XDISPLOC
				else if( opt == 5 ) // ??
					sendbuf += System.format("%c%c%c", 0xFF, 254, opt); // do NOT STATUS
				else if( opt == 39 ) // ??
					sendbuf += System.format("%c%c%c", 0xFF, 254, opt); // do NOT NEW-ENVIRON
			}
		}
		else
			recvbuf += data.charAt(x);
	}
	//puts("");

	if( sendbuf.length > 0 )
		net.send( id, sendbuf );

	if( recvbuf.length == 0 )
	{
		// Just telnet options.
		return true;
	}

	//puts( cprint("[blue]([!blue]RECV[!white]:[white]"+id+"[blue]) [cyan]" + recvbuf) );
	if( client[id]['handler'] && client[id]['handler']['onRecv'] )
		client[id]['handler']['onRecv']( id, recvbuf );

	return true;
}

function handleClose( s:userobj|null, id:number )
{
	console.log("Close from: "+JSON.stringify(s));
	console.log("Close ID: "+id);
	if( client[id]['door'] )
	{
		client[id]['door'].close();
		delete client[id]['door'];
	}
	return true;
}

var net = new Socket( { 'port':23, 'onRecv':handleRecv, 'onOpen':handleOpen, 'onClose':handleClose, 'noAsync':true } );

System.update(-1);

