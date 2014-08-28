#include <authorization/constants.h>
#include <authorization/authorization.h>
#include <oak/oak.h>
#include <text/format.h>
#include <cf/cf.h>
#include <io/path.h>
#include <plist/uuid.h>

static double const AppVersion  = 2.7;
static size_t const AppRevision = APP_REVISION;

static char const* socket_path ()
{
	int uid = getuid();
	if(getenv("SUDO_UID"))
		uid = atoi(getenv("SUDO_UID"));

	static std::string const str = text::format("/tmp/textmate-%d.sock", uid);
	return str.c_str();
}

// mate returns when all files specified has been opened.
// If - is used for filename, stdin is read and opened as a new buffer.
// If -w is given, mate waits for all files to be closed, before it returns.
// If -w is used with -, the buffer is written to stdout
// If -w is used without any file names, - is implied

struct disable_sudo_helper_t
{
	disable_sudo_helper_t () : _uid(geteuid()), _gid(getegid())
	{
		if(_uid != 0)
			return;

		if(getenv("SUDO_UID") && getenv("SUDO_GID"))
		{
			setegid(atoi(getenv("SUDO_GID")));
			seteuid(atoi(getenv("SUDO_UID")));
		}
	}

	~disable_sudo_helper_t ()
	{
		if(_uid != 0)
			return;

		seteuid(_uid);
		setegid(_gid);
	}

private:
	uid_t _uid;
	gid_t _gid;
};

static bool find_app (FSRef* outAppRef, std::string* outAppStr)
{
	disable_sudo_helper_t helper;

	CFURLRef appURL;
	OSStatus err = LSFindApplicationForInfo(kLSUnknownCreator, CFSTR("com.macromates.TextMate.preview"), NULL, outAppRef, &appURL);
	if(err != noErr)
		return fprintf(stderr, "Can’t find TextMate.app (error %d)\n", (int)err), false;

	if(outAppStr)
	{
		if(CFStringRef appPath = CFURLCopyFileSystemPath(appURL, kCFURLPOSIXPathStyle))
		{
			*outAppStr = cf::to_s(appPath);
			CFRelease(appPath);
		}
		CFRelease(appURL);
	}

	return err == noErr;
}

static void launch_app (bool disableUntitled)
{
	disable_sudo_helper_t helper;

	FSRef appFSRef;
	if(!find_app(&appFSRef, NULL))
		exit(EX_UNAVAILABLE);

	cf::array_t args(disableUntitled ? std::vector<std::string>{ "-disableNewDocumentAtStartup", "1" } : std::vector<std::string>{ });

	struct LSApplicationParameters const appParams = { 0, kLSLaunchDontAddToRecents|kLSLaunchDontSwitch|kLSLaunchAndDisplayErrors, &appFSRef, NULL, NULL, args, NULL };
	OSStatus err = LSOpenApplication(&appParams, NULL);
	if(err != noErr)
	{
		fprintf(stderr, "Can’t launch TextMate.app (error %d)", (int)err);
		exit(EX_UNAVAILABLE);
	}
}

static void install_auth_tool ()
{
	if(geteuid() == 0 && (!path::exists(kAuthToolPath) || !path::exists(kAuthPlistPath) || AuthorizationRightGet(kAuthRightName, NULL) == errAuthorizationDenied))
	{
		std::string appStr = NULL_STR;
		if(!find_app(NULL, &appStr))
			exit(EX_UNAVAILABLE);

		std::string toolPath = path::join(appStr, "Contents/Resources/PrivilegedTool");
		char const* arg0 = toolPath.c_str();

		if(access(arg0, X_OK) != 0)
		{
			fprintf(stderr, "No such executable file: ‘%s’\n", arg0);
			exit(EX_UNAVAILABLE);
		}

		pid_t pid = vfork();
		if(pid == 0)
		{
			execl(arg0, arg0, "--install", NULL);
			_exit(errno);
		}

		if(pid != -1)
		{
			int status = 0;
			if(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) != 0)
				fprintf(stderr, "%s: %s\n", arg0, strerror(WEXITSTATUS(status)));
		}
	}
}

static void usage (FILE* io)
{
	std::string pad(8 - std::min(strlen(getprogname()), size_t(8)), ' ');

	fprintf(io,
		"%1$s %2$.1f (" COMPILE_DATE " revision %3$zu)\n"
		"Usage: %1$s [-wl<number>t<filetype>rdnhv] [file ...]\n"
		"Options:\n"
		" -w, --[no-]wait        Wait for file to be closed by TextMate.\n"
		" -l, --line <number>    Place caret on line <number> after loading file.\n"
		" -t, --type <filetype>  Treat file as having <filetype>.\n"
		" -m, --name <name>      The display name shown in TextMate.\n"
		" -r, --[no-]recent      Add file to Open Recent menu.\n"
		" -d, --change-dir       Change TextMate's working directory to that of the file.\n"
		" -u, --uuid             Reference an already open document using its UUID.\n"
		" -e, --[no-]escapes     Set this if you want ANSI escapes from stdin to be preserved.\n"
		" -h, --help             Show this information.\n"
		" -v, --version          Print version information.\n"
		"\n"
		"By default %1$s will wait for files to be closed if the command name\n"
		"has a \"_wait\" suffix (e.g. via a symbolic link) or when used as a\n"
		"filter like in this examples:\n"
		"\n"
		"    ls *.tex|%1$s|sh%4$s-w implied\n"
		"    %1$s -|cat -n   %4$s-w implied (read from stdin)\n"
		"\n", getprogname(), AppVersion, AppRevision, pad.c_str()
	);
}

static void version ()
{
	fprintf(stdout, "%1$s %2$.1f (" COMPILE_DATE " revision %3$zu)\n", getprogname(), AppVersion, AppRevision);
}

static void append (std::string const& str, std::vector<std::string>& v)
{
	std::string::size_type from = 0;
	while(from < str.size() && from != std::string::npos)
	{
		std::string::size_type to = str.find(',', from);
		v.push_back(std::string(str.begin() + from, to == std::string::npos ? str.end() : str.begin() + to));
		from = to == std::string::npos ? to : to + 1;
	}
}

static void write_key_pair (int fd, std::string const& key, std::string const& value)
{
	std::string const str = key + ": " + value + "\r\n";
	write(fd, str.data(), str.size());
}

static std::string const kUUIDPrefix = "uuid://";

namespace
{
	enum class boolean {
		kUnset, kEnable, kDisable
	};

	std::string to_s (boolean flag)
	{
		return flag == boolean::kEnable ? "yes" : "no";
	}

	enum class escape_state_t {
		kPlain, kEscape, kANSI
	};
}

template <typename _InputIter>
_InputIter remove_ansi_escapes (_InputIter it, _InputIter last, escape_state_t* state)
{
	auto dst = it;
	for(; it != last; ++it)
	{
		auto const& ch = *it;
		switch(*state)
		{
			case escape_state_t::kPlain:
			{
				if(ch == '\e')
					*state = escape_state_t::kEscape;
				else if(it == dst)
					++dst;
				else
					*dst++ = *it;
			}
			break;

			case escape_state_t::kEscape:
			{
				if(ch == '[')
						*state = escape_state_t::kANSI;
				else	*state = escape_state_t::kPlain;
			}
			break;

			case escape_state_t::kANSI:
			{
				if(0x40 <= ch && ch <= 0x7E)
					*state = escape_state_t::kPlain;
			}
			break;
		}
	}
	return dst;
}

int main (int argc, char* argv[])
{
	extern char* optarg;
	extern int optind;

	static struct option const longopts[] = {
		{ "async",            no_argument,         0,      'a'   },
		{ "change-dir",       no_argument,         0,      'd'   },
		{ "escapes",          no_argument,         0,      'e'   },
		{ "no-escapes",       no_argument,         0,      'E'   },
		{ "help",             no_argument,         0,      'h'   },
		{ "line",             required_argument,   0,      'l'   },
		{ "name",             required_argument,   0,      'm'   },
		{ "project",          required_argument,   0,      'p'   },
		{ "recent",           no_argument,         0,      'r'   },
		{ "no-recent",        no_argument,         0,      'R'   },
		{ "type",             required_argument,   0,      't'   },
		{ "uuid",             required_argument,   0,      'u'   },
		{ "version",          no_argument,         0,      'v'   },
		{ "wait",             no_argument,         0,      'w'   },
		{ "no-wait",          no_argument,         0,      'W'   },
		{ 0,                  0,                   0,      0     }
	};

	osx::authorization_t auth;
	std::vector<std::string> files, lines, types, names, projects;
	oak::uuid_t uuid;

	boolean changeDir   = boolean::kDisable;
	boolean shouldWait  = boolean::kUnset;
	boolean addToRecent = boolean::kUnset;
	boolean keepEscapes = boolean::kUnset;

	if(strlen(getprogname()) > 5 && strcmp(getprogname() + strlen(getprogname()) - 5, "_wait") == 0)
		shouldWait = boolean::kEnable;

	install_auth_tool();

	int ch;
	while((ch = getopt_long(argc, argv, "adehl:m:p:rst:u:vw", longopts, NULL)) != -1)
	{
		switch(ch)
		{
			case 'a': shouldWait = boolean::kDisable;  break;
			case 'd': changeDir = boolean::kEnable;    break;
			case 'e': keepEscapes = boolean::kEnable;  break;
			case 'E': keepEscapes = boolean::kDisable; break;
			case 'h': usage(stdout);            return EX_OK;
			case 'l': append(optarg, lines);    break;
			case 'm': append(optarg, names);    break;
			case 'p': append(optarg, projects); break;
			case 'r': addToRecent = boolean::kEnable;  break;
			case 'R': addToRecent = boolean::kDisable; break;
			case 't': append(optarg, types);    break;
			case 'u': uuid = optarg;            break;
			case 'v': version();                return EX_OK;
			case 'w': shouldWait = boolean::kEnable;  break;
			case 'W': shouldWait = boolean::kDisable; break;
			case '?': /* unknown option */      return EX_USAGE;
			case ':': /* missing option */      return EX_USAGE;
			default:  usage(stderr);            return EX_USAGE;
		}
	}

	argc -= optind;
	argv += optind;

	for(int i = 0; i < argc; ++i)
	{
		char* path = argv[i];
		if(path[0] == 0)
			continue;

		if(strcmp(path, "-") != 0 && !path::is_absolute(path)) // relative path, make absolute
		{
			if(char* cwd = getcwd(NULL, (size_t)-1))
			{
				asprintf(&path, "%s/%s", cwd, path);
				free(cwd);
			}
			else
			{
				fprintf(stderr, "failed to get current working directory\n");
				exit(EX_OSERR);
			}
		}

		files.push_back(path);
	}

	std::string defaultProject = projects.empty() ? (getenv("TM_PROJECT_UUID") ?: "") : projects.back();

	bool stdinIsAPipe = isatty(STDIN_FILENO) == 0;
	if(files.empty())
	{
		if(uuid)
			files.push_back(kUUIDPrefix + to_s(uuid));
		else if(shouldWait == boolean::kEnable || stdinIsAPipe)
			files.push_back("-");
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = { 0, AF_UNIX };
	strcpy(addr.sun_path, socket_path());
	addr.sun_len = SUN_LEN(&addr);

	bool didLaunch = false;
	while(-1 == connect(fd, (sockaddr*)&addr, sizeof(addr)))
	{
		if(!didLaunch)
			launch_app(!files.empty());
		didLaunch = true;
		usleep(500000);
	}

	char buf[1024];
	ssize_t len = read(fd, buf, sizeof(buf));
	if(len == -1)
		exit(EX_IOERR);

	for(size_t i = 0; i < files.size(); ++i)
	{
		write(fd, "open\r\n", 6);

		if(files[i] == "-")
		{
			if(!stdinIsAPipe)
				fprintf(stderr, "Reading from stdin, press ^D to stop\n");

			ssize_t total = 0;
			bool didStripEscapes = false;
			escape_state_t state = escape_state_t::kPlain;
			while(ssize_t len = read(STDIN_FILENO, buf, sizeof(buf)))
			{
				if(len == -1)
					break;

				if(keepEscapes != boolean::kEnable)
				{
					size_t oldLen = std::exchange(len, remove_ansi_escapes(buf, buf + len, &state) - buf);
					didStripEscapes = didStripEscapes || oldLen != len;
				}

				write_key_pair(fd, "data", std::to_string(len));
				total += len;
				write(fd, buf, len);
			}

			if(didStripEscapes && keepEscapes == boolean::kUnset)
				fprintf(stderr, "WARNING: Removed ANSI escape codes. Use -e/--[no-]escapes.\n");

			if(stdinIsAPipe && total == 0 && shouldWait != boolean::kEnable && getenv("TM_DOCUMENT_UUID"))
			{
				write_key_pair(fd, "uuid", getenv("TM_DOCUMENT_UUID"));
			}
			else
			{
				bool stdoutIsAPipe = isatty(STDOUT_FILENO) == 0;
				bool wait = shouldWait == boolean::kEnable || (shouldWait == boolean::kUnset && stdoutIsAPipe);
				write_key_pair(fd, "display-name",        i < names.size()      ? names[i] : "untitled (stdin)");
				write_key_pair(fd, "data-on-close",       wait && stdoutIsAPipe ? "yes" : "no");
				write_key_pair(fd, "wait",                wait                  ? "yes" : "no");
				write_key_pair(fd, "re-activate",         wait                  ? "yes" : "no");
			}
		}
		else if(files[i].find(kUUIDPrefix) == 0)
		{
			write_key_pair(fd, "uuid", files[i].substr(kUUIDPrefix.size()));
		}
		else
		{
			write_key_pair(fd, "path",             files[i]);
			write_key_pair(fd, "display-name",     i < names.size() ? names[i] : "");
			write_key_pair(fd, "wait",             to_s(shouldWait));
			write_key_pair(fd, "re-activate",      to_s(shouldWait));
		}

		if(geteuid() == 0 && auth.obtain_right(kAuthRightName))
			write_key_pair(fd, "authorization", auth);

		write_key_pair(fd, "selection",        i < lines.size()    ? lines[i] : "");
		write_key_pair(fd, "file-type",        i < types.size()    ? types[i] : "");
		write_key_pair(fd, "project-uuid",     i < projects.size() ? projects[i] : defaultProject);
		write_key_pair(fd, "add-to-recents",   to_s(addToRecent));
		write_key_pair(fd, "change-directory", to_s(changeDir));

		write(fd, "\r\n", 2);
	}

	write(fd, ".\r\n", 3);

	// =========================
	// = Now wait for messages =
	// =========================

	enum { command, arguments, data, done } state = command;
	ssize_t bytesLeft = 0;

	std::string line;
	while(state != done && (len = read(fd, buf, sizeof(buf))))
	{
		if(len == -1)
		{
			perror("read()");
			break;
		}

		if(state == data)
		{
			ssize_t dataLen = std::min(len, bytesLeft);
			write(STDOUT_FILENO, buf, dataLen);
			memmove(buf, buf + dataLen, len - dataLen);
			bytesLeft -= dataLen;
			len -= dataLen;

			state = bytesLeft == 0 ? arguments : data;
		}

		line.insert(line.end(), buf, buf + len);

		if(state == data && bytesLeft != 0)
			continue;

		while(line.find('\n') != std::string::npos)
		{
			std::string::size_type eol = line.find('\n');
			std::string str = line.substr(0, eol);
			if(!str.empty() && str.back() == '\r')
				str.resize(str.size()-1);
			line.erase(line.begin(), line.begin() + eol + 1);

			if(str.empty())
			{
				state = command;
			}
			else if(state == command)
			{
				if(str == "close")
				{
					state = arguments;
				}
			}
			else if(state == arguments)
			{
				std::string::size_type n = str.find(':');
				if(n != std::string::npos)
				{
					std::string key   = str.substr(0, n);
					std::string value = str.substr(n+2);

					if(key == "data")
					{
						bytesLeft = strtol(value.c_str(), NULL, 10);

						size_t dataLen = std::min((ssize_t)line.size(), bytesLeft);
						write(STDOUT_FILENO, line.data(), dataLen);
						line.erase(line.begin(), line.begin() + dataLen);
						bytesLeft -= dataLen;

						state = bytesLeft == 0 ? arguments : data;
					}
				}
			}
		}
	}

	close(fd);
	return EX_OK;
}
