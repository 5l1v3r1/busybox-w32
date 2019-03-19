/*
 * Copyright 2008 Peter Harris <git@peter.is-a-geek.org>
 */

#include "libbb.h"
#include <windows.h>
#undef PACKED

/*
 Functions to be wrapped:
*/
#undef vfprintf
#undef vprintf
#undef printf
#undef fprintf
#undef fputs
#undef putchar
#undef fwrite
#undef puts
#undef write
#undef read
#undef getc

/*
 ANSI codes used by git: m, K

 This file is git-specific. Therefore, this file does not attempt
 to implement any codes that are not used by git.
*/

static HANDLE console;
static HANDLE console_in;
static WORD plain_attr;
static WORD attr;
static int negative;

static void init(void)
{
	CONSOLE_SCREEN_BUFFER_INFO sbi;

	static int initialized = 0;
	if (initialized)
		return;

	console_in = GetStdHandle(STD_INPUT_HANDLE);
	if (console_in == INVALID_HANDLE_VALUE)
		console_in = NULL;

	console = GetStdHandle(STD_OUTPUT_HANDLE);
	if (console == INVALID_HANDLE_VALUE)
		console = NULL;

	if (!console)
		return;

	GetConsoleScreenBufferInfo(console, &sbi);
	attr = plain_attr = sbi.wAttributes;
	negative = 0;

	initialized = 1;
}

static int is_console(int fd)
{
	init();
	return isatty(fd) && console;
}

static int is_console_in(int fd)
{
	init();
	return isatty(fd) && console_in;
}

static int skip_ansi_emulation(void)
{
	static char *var = NULL;
	static int got_var = FALSE;

	if (!got_var) {
		var = getenv("BB_SKIP_ANSI_EMULATION");
		got_var = TRUE;
	}

	return var != NULL;
}

void set_title(const char *str)
{
	if (is_console(STDOUT_FILENO))
		SetConsoleTitle(str);
}


#define FOREGROUND_ALL (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define BACKGROUND_ALL (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE)

static void set_console_attr(void)
{
	WORD attributes = attr;
	if (negative) {
		attributes &= ~FOREGROUND_ALL;
		attributes &= ~BACKGROUND_ALL;

		/* This could probably use a bitmask
		   instead of a series of ifs */
		if (attr & FOREGROUND_RED)
			attributes |= BACKGROUND_RED;
		if (attr & FOREGROUND_GREEN)
			attributes |= BACKGROUND_GREEN;
		if (attr & FOREGROUND_BLUE)
			attributes |= BACKGROUND_BLUE;

		if (attr & BACKGROUND_RED)
			attributes |= FOREGROUND_RED;
		if (attr & BACKGROUND_GREEN)
			attributes |= FOREGROUND_GREEN;
		if (attr & BACKGROUND_BLUE)
			attributes |= FOREGROUND_BLUE;
	}
	SetConsoleTextAttribute(console, attributes);
}

static void erase_in_line(void)
{
	CONSOLE_SCREEN_BUFFER_INFO sbi;
	DWORD dummy; /* Needed for Windows 7 (or Vista) regression */

	if (!console)
		return;

	GetConsoleScreenBufferInfo(console, &sbi);
	FillConsoleOutputCharacterA(console, ' ',
		sbi.dwSize.X - sbi.dwCursorPosition.X, sbi.dwCursorPosition,
		&dummy);
	FillConsoleOutputAttribute(console, plain_attr,
		sbi.dwSize.X - sbi.dwCursorPosition.X, sbi.dwCursorPosition,
		&dummy);
}

static void erase_till_end_of_screen(void)
{
	CONSOLE_SCREEN_BUFFER_INFO sbi;
	DWORD dummy, len;

	if (!console)
		return;

	GetConsoleScreenBufferInfo(console, &sbi);
	len = sbi.dwSize.X - sbi.dwCursorPosition.X +
			sbi.dwSize.X * (sbi.srWindow.Bottom - sbi.dwCursorPosition.Y);

	FillConsoleOutputCharacterA(console, ' ', len, sbi.dwCursorPosition,
		&dummy);
	FillConsoleOutputAttribute(console, plain_attr, len, sbi.dwCursorPosition,
		&dummy);
}

void reset_screen(void)
{
	CONSOLE_SCREEN_BUFFER_INFO sbi;
	COORD pos;
	DWORD dummy, len;

	if (!console)
		return;

	/* move to start of screen buffer and clear it all */
	GetConsoleScreenBufferInfo(console, &sbi);
	pos.X = 0;
	pos.Y = 0;
	SetConsoleCursorPosition(console, pos);
	len = sbi.dwSize.X * sbi.dwSize.Y;
	FillConsoleOutputCharacterA(console, ' ', len, pos, &dummy);
	FillConsoleOutputAttribute(console, plain_attr, len, pos, &dummy);
}

void move_cursor_row(int n)
{
	CONSOLE_SCREEN_BUFFER_INFO sbi;

	if (!console)
		return;

	GetConsoleScreenBufferInfo(console, &sbi);
	sbi.dwCursorPosition.Y += n;
	SetConsoleCursorPosition(console, sbi.dwCursorPosition);
}

static void move_cursor_column(int n)
{
	CONSOLE_SCREEN_BUFFER_INFO sbi;

	if (!console)
		return;

	GetConsoleScreenBufferInfo(console, &sbi);
	sbi.dwCursorPosition.X += n;
	SetConsoleCursorPosition(console, sbi.dwCursorPosition);
}

static void move_cursor(int x, int y)
{
	COORD pos;
	CONSOLE_SCREEN_BUFFER_INFO sbi;

	if (!console)
		return;

	GetConsoleScreenBufferInfo(console, &sbi);
	pos.X = sbi.srWindow.Left + x;
	pos.Y = sbi.srWindow.Top + y;
	SetConsoleCursorPosition(console, pos);
}

/* On input pos points to the start of a suspected escape sequence.
 * If a valid sequence is found return a pointer to the character
 * following it, otherwise return the original pointer. */
static char *process_escape(char *pos)
{
	const char *str, *func;
	char *bel;
	size_t len;

	switch (pos[1]) {
	case '[':
		/* go ahead and process "\033[" sequence */
		break;
	case ']':
		if ((pos[2] == '0' || pos[2] == '2') && pos[3] == ';' &&
				(bel=strchr(pos+4, '\007')) && bel - pos < 260) {
			/* set console title */
			*bel++ = '\0';
			CharToOem(pos+4, pos+4);
			SetConsoleTitle(pos+4);
			return bel;
		}
		/* invalid "\033]" sequence, fall through */
	default:
		return pos;
	}

	str = pos + 2;
	len = strspn(str, "0123456789;");
	func = str + len;
	switch (*func) {
	case 'm':
		do {
			long val = strtol(str, (char **)&str, 10);
			switch (val) {
			case 0: /* reset */
				attr = plain_attr;
				negative = 0;
				break;
			case 1: /* bold */
				attr |= FOREGROUND_INTENSITY;
				break;
			case 2:  /* faint */
			case 22: /* normal */
				attr &= ~FOREGROUND_INTENSITY;
				break;
			case 3:  /* italic */
				/* Unsupported */
				break;
			case 4:  /* underline */
			case 21: /* double underline */
				/* Wikipedia says this flag does nothing */
				/* Furthermore, mingw doesn't define this flag
				attr |= COMMON_LVB_UNDERSCORE; */
				break;
			case 24: /* no underline */
				/* attr &= ~COMMON_LVB_UNDERSCORE; */
				break;
			case 5:  /* slow blink */
			case 6:  /* fast blink */
				/* We don't have blink, but we do have
				   background intensity */
				attr |= BACKGROUND_INTENSITY;
				break;
			case 25: /* no blink */
				attr &= ~BACKGROUND_INTENSITY;
				break;
			case 7:  /* negative */
				negative = 1;
				break;
			case 27: /* positive */
				negative = 0;
				break;
			case 8:  /* conceal */
			case 28: /* reveal */
				/* Unsupported */
				break;
			case 30: /* Black */
				attr &= ~FOREGROUND_ALL;
				break;
			case 31: /* Red */
				attr &= ~FOREGROUND_ALL;
				attr |= FOREGROUND_RED;
				break;
			case 32: /* Green */
				attr &= ~FOREGROUND_ALL;
				attr |= FOREGROUND_GREEN;
				break;
			case 33: /* Yellow */
				attr &= ~FOREGROUND_ALL;
				attr |= FOREGROUND_RED | FOREGROUND_GREEN;
				break;
			case 34: /* Blue */
				attr &= ~FOREGROUND_ALL;
				attr |= FOREGROUND_BLUE;
				break;
			case 35: /* Magenta */
				attr &= ~FOREGROUND_ALL;
				attr |= FOREGROUND_RED | FOREGROUND_BLUE;
				break;
			case 36: /* Cyan */
				attr &= ~FOREGROUND_ALL;
				attr |= FOREGROUND_GREEN | FOREGROUND_BLUE;
				break;
			case 37: /* White */
				attr |= FOREGROUND_RED |
					FOREGROUND_GREEN |
					FOREGROUND_BLUE;
				break;
			case 38: /* Unknown */
				break;
			case 39: /* reset */
				attr &= ~FOREGROUND_ALL;
				attr |= (plain_attr & FOREGROUND_ALL);
				break;
			case 40: /* Black */
				attr &= ~BACKGROUND_ALL;
				break;
			case 41: /* Red */
				attr &= ~BACKGROUND_ALL;
				attr |= BACKGROUND_RED;
				break;
			case 42: /* Green */
				attr &= ~BACKGROUND_ALL;
				attr |= BACKGROUND_GREEN;
				break;
			case 43: /* Yellow */
				attr &= ~BACKGROUND_ALL;
				attr |= BACKGROUND_RED | BACKGROUND_GREEN;
				break;
			case 44: /* Blue */
				attr &= ~BACKGROUND_ALL;
				attr |= BACKGROUND_BLUE;
				break;
			case 45: /* Magenta */
				attr &= ~BACKGROUND_ALL;
				attr |= BACKGROUND_RED | BACKGROUND_BLUE;
				break;
			case 46: /* Cyan */
				attr &= ~BACKGROUND_ALL;
				attr |= BACKGROUND_GREEN | BACKGROUND_BLUE;
				break;
			case 47: /* White */
				attr |= BACKGROUND_RED |
					BACKGROUND_GREEN |
					BACKGROUND_BLUE;
				break;
			case 48: /* Unknown */
				break;
			case 49: /* reset */
				attr &= ~BACKGROUND_ALL;
				attr |= (plain_attr & BACKGROUND_ALL);
				break;
			default:
				/* Unsupported code */
				return pos;
			}
			str++;
		} while (*(str-1) == ';');

		set_console_attr();
		break;
	case 'A': /* up */
		move_cursor_row(-strtol(str, (char **)&str, 10));
		break;
	case 'B': /* down */
		move_cursor_row(strtol(str, (char **)&str, 10));
		break;
	case 'C': /* forward */
		move_cursor_column(strtol(str, (char **)&str, 10));
		break;
	case 'D': /* back */
		move_cursor_column(-strtol(str, (char **)&str, 10));
		break;
	case 'H':
		if (!len)
			move_cursor(0, 0);
		else {
			int row, col = 1;

			row = strtol(str, (char **)&str, 10);
			if (*str == ';') {
				col = strtol(str+1, (char **)&str, 10);
			}
			move_cursor(col > 0 ? col-1 : 0, row > 0 ? row-1 : 0);
		}
		break;
	case 'J':
		erase_till_end_of_screen();
		break;
	case 'K':
		erase_in_line();
		break;
	case '?':
		/* skip this to avoid ugliness when vi is shut down */
		++str;
		while (isdigit(*str))
			++str;
		func = str;
		break;
	default:
		/* Unsupported code */
		return pos;
	}

	return (char *)func + 1;
}

#if ENABLE_FEATURE_EURO
void init_codepage(void)
{
	if (GetConsoleCP() == 850 && GetConsoleOutputCP() == 850) {
		SetConsoleCP(858);
		SetConsoleOutputCP(858);
	}
}

static BOOL winansi_CharToOemBuff(LPCSTR s, LPSTR d, DWORD len)
{
	WCHAR *buf;
	int i;

	if (!s || !d)
		return FALSE;

	buf = xmalloc(len*sizeof(WCHAR));
	MultiByteToWideChar(CP_ACP, 0, s, len, buf, len);
	WideCharToMultiByte(CP_OEMCP, 0, buf, len, d, len, NULL, NULL);
	if (GetConsoleOutputCP() == 858) {
		for (i=0; i<len; ++i) {
			if (buf[i] == 0x20ac) {
				d[i] = 0xd5;
			}
		}
	}
	free(buf);
	return TRUE;
}

static BOOL winansi_CharToOem(LPCSTR s, LPSTR d)
{
	if (!s || !d)
		return FALSE;
	return winansi_CharToOemBuff(s, d, strlen(s)+1);
}

static BOOL winansi_OemToCharBuff(LPCSTR s, LPSTR d, DWORD len)
{
	WCHAR *buf;
	int i;

	if (!s || !d)
		return FALSE;

	buf = xmalloc(len*sizeof(WCHAR));
	MultiByteToWideChar(CP_OEMCP, 0, s, len, buf, len);
	WideCharToMultiByte(CP_ACP, 0, buf, len, d, len, NULL, NULL);
	if (GetConsoleOutputCP() == 858) {
		for (i=0; i<len; ++i) {
			if (buf[i] == 0x0131) {
				d[i] = 0x80;
			}
		}
	}
	free(buf);
	return TRUE;
}

# undef CharToOemBuff
# undef CharToOem
# undef OemToCharBuff
# define CharToOemBuff winansi_CharToOemBuff
# define CharToOem winansi_CharToOem
# define OemToCharBuff winansi_OemToCharBuff
#endif

static int ansi_emulate(const char *s, FILE *stream)
{
	int rv = 0;
	const unsigned char *t;
	char *pos, *str;
	size_t cur_len;
	static size_t max_len = 0;
	static char *mem = NULL;

	/* if no special treatment is required output the string as-is */
	for ( t=(unsigned char *)s; *t; ++t ) {
		if ( *t == '\033' || *t > 0x7f ) {
			break;
		}
	}

	if ( *t == '\0' ) {
		return fputs(s, stream) == EOF ? EOF : strlen(s);
	}

	/*
	 * Make a writable copy of the string and retain array for reuse.
	 * The test above guarantees that the string length won't be zero
	 * so the array will always be allocated.
	 */
	cur_len = strlen(s);
	if ( cur_len > max_len ) {
		free(mem);
		mem = strdup(s);
		max_len = cur_len;
	}
	else {
		strcpy(mem, s);
	}
	pos = str = mem;

	while (*pos) {
		pos = strchr(str, '\033');
		if (pos && !skip_ansi_emulation()) {
			size_t len = pos - str;

			if (len) {
				*pos = '\0';	/* NB, '\033' has been overwritten */
				CharToOem(str, str);
				if (fputs(str, stream) == EOF)
					return EOF;
				rv += len;
			}

			if (fflush(stream) == EOF)
				return EOF;

			str = process_escape(pos);
			if (str == pos) {
				if (fputc('\033', stream) == EOF)
					return EOF;
				++str;
			}
			rv += str - pos;
			pos = str;

			if (fflush(stream) == EOF)
				return EOF;

		} else {
			rv += strlen(str);
			CharToOem(str, str);
			return fputs(str, stream) == EOF ? EOF : rv;
		}
	}
	return rv;
}

int winansi_putchar(int c)
{
	char t = c;
	char *s = &t;

	if (!is_console(STDOUT_FILENO))
		return putchar(c);

	CharToOemBuff(s, s, 1);
	return putchar(t) == EOF ? EOF : c;
}

int winansi_puts(const char *s)
{
	return (winansi_fputs(s, stdout) == EOF || putchar('\n') == EOF) ? EOF : 0;
}

static sighandler_t sigpipe_handler = SIG_DFL;

#undef signal
sighandler_t winansi_signal(int signum, sighandler_t handler)
{
	sighandler_t old;

	if (signum == SIGPIPE) {
		old = sigpipe_handler;
		sigpipe_handler = handler;
		return old;
	}
	return signal(signum, handler);
}

static void check_pipe_fd(int fd)
{
	int error = GetLastError();

	if ((error == ERROR_NO_DATA &&
			GetFileType((HANDLE)_get_osfhandle(fd)) == FILE_TYPE_PIPE) ||
			error == ERROR_BROKEN_PIPE) {
		if (sigpipe_handler == SIG_DFL)
			exit(128+SIGPIPE);
		else /* SIG_IGN */
			errno = EPIPE;
	}
}

static void check_pipe(FILE *stream)
{
	int fd = fileno(stream);

	if (fd != -1 && ferror(stream)) {
		check_pipe_fd(fd);
	}
}

size_t winansi_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t lsize, lmemb, ret;
	char *str;
	int rv;

	lsize = MIN(size, nmemb);
	lmemb = MAX(size, nmemb);
	if (lsize != 1 || !is_console(fileno(stream))) {
		SetLastError(0);
		if ((ret=fwrite(ptr, size, nmemb, stream)) < nmemb)
			check_pipe(stream);
		return ret;
	}

	str = xmalloc(lmemb+1);
	memcpy(str, ptr, lmemb);
	str[lmemb] = '\0';

	rv = ansi_emulate(str, stream);
	free(str);

	return rv == EOF ? 0 : nmemb;
}

int winansi_fputs(const char *str, FILE *stream)
{
	int ret;

	if (!is_console(fileno(stream))) {
		SetLastError(0);
		if ((ret=fputs(str, stream)) == EOF)
			check_pipe(stream);
		return ret;
	}

	return ansi_emulate(str, stream) == EOF ? EOF : 0;
}

#if !defined(__USE_MINGW_ANSI_STDIO) || !__USE_MINGW_ANSI_STDIO
/*
 * Prior to Windows 10 vsnprintf was incompatible with the C99 standard.
 * Implement a replacement using _vsnprintf.
 */
int winansi_vsnprintf(char *buf, size_t size, const char *format, va_list list)
{
	size_t len;
	va_list list2;

	va_copy(list2, list);
	len = _vsnprintf(NULL, 0, format, list2);
	if (len < 0)
		return -1;

	_vsnprintf(buf, size, format, list);
	buf[size-1] = '\0';
	return len;
}
#endif

int winansi_vfprintf(FILE *stream, const char *format, va_list list)
{
	int len, rv;
	char small_buf[256];
	char *buf = small_buf;
	va_list cp;

	if (!is_console(fileno(stream)))
		goto abort;

	va_copy(cp, list);
	len = vsnprintf(small_buf, sizeof(small_buf), format, cp);
	va_end(cp);

	if (len > sizeof(small_buf) - 1) {
		buf = malloc(len + 1);
		if (!buf)
			goto abort;

		va_copy(cp, list);
		len = vsnprintf(buf, len + 1, format, cp);
		va_end(cp);
	}

	if (len == -1)
		goto abort;

	rv = ansi_emulate(buf, stream);

	if (buf != small_buf)
		free(buf);
	return rv;

abort:
	SetLastError(0);
	if ((rv=vfprintf(stream, format, list)) == EOF)
		check_pipe(stream);
	return rv;
}

int winansi_fprintf(FILE *stream, const char *format, ...)
{
	va_list list;
	int rv;

	va_start(list, format);
	rv = winansi_vfprintf(stream, format, list);
	va_end(list);

	return rv;
}

int winansi_printf(const char *format, ...)
{
	va_list list;
	int rv;

	va_start(list, format);
	rv = winansi_vfprintf(stdout, format, list);
	va_end(list);

	return rv;
}

static int ansi_emulate_write(int fd, const void *buf, size_t count)
{
	int rv = 0, i;
	int special = FALSE, has_null = FALSE;
	const unsigned char *s = (const unsigned char *)buf;
	char *pos, *str;
	size_t len, out_len;
	static size_t max_len = 0;
	static char *mem = NULL;

	for ( i=0; i<count; ++i ) {
		if ( s[i] == '\033' || s[i] > 0x7f ) {
			special = TRUE;
		}
		else if ( !s[i] ) {
			has_null = TRUE;
		}
	}

	/*
	 * If no special treatment is required or the data contains NUL
	 * characters output the string as-is.
	 */
	if ( !special || has_null ) {
		return write(fd, buf, count);
	}

	/* make a writable copy of the data and retain array for reuse */
	if ( count > max_len ) {
		free(mem);
		mem = malloc(count+1);
		max_len = count;
	}
	memcpy(mem, buf, count);
	mem[count] = '\0';
	pos = str = mem;

	/* we've checked the data doesn't contain any NULs */
	while (*pos) {
		pos = strchr(str, '\033');
		if (pos && !skip_ansi_emulation()) {
			len = pos - str;

			if (len) {
				CharToOemBuff(str, str, len);
				out_len = write(fd, str, len);
				if (out_len == -1)
					return -1;
				rv += out_len;
			}

			str = process_escape(pos);
			if (str == pos) {
				if (write(fd, pos, 1) == -1)
					return -1;
				++str;
			}
			rv += str - pos;
			pos = str;
		} else {
			len = strlen(str);
			CharToOem(str, str);
			out_len = write(fd, str, len);
			return (out_len == -1) ? -1 : rv+out_len;
		}
	}
	return rv;
}

int winansi_write(int fd, const void *buf, size_t count)
{
	if (!is_console(fd)) {
		int ret;

		SetLastError(0);
		if ((ret=write(fd, buf, count)) == -1) {
			check_pipe_fd(fd);
		}
		return ret;
	}

	return ansi_emulate_write(fd, buf, count);
}

int winansi_read(int fd, void *buf, size_t count)
{
	int rv;

	rv = mingw_read(fd, buf, count);
	if (!is_console_in(fd))
		return rv;

	if ( rv > 0 ) {
		OemToCharBuff(buf, buf, rv);
	}

	return rv;
}

int winansi_getc(FILE *stream)
{
	int rv;

	rv = getc(stream);
	if (!is_console_in(fileno(stream)))
		return rv;

	if ( rv != EOF ) {
		unsigned char c = (unsigned char)rv;
		char *s = (char *)&c;
		OemToCharBuff(s, s, 1);
		rv = (int)c;
	}

	return rv;
}

/* Ensure that isatty(fd) returns 0 for the NUL device */
int mingw_isatty(int fd)
{
	int result = _isatty(fd);

	if (result) {
		HANDLE handle = (HANDLE) _get_osfhandle(fd);
		DWORD mode;

		if (handle == INVALID_HANDLE_VALUE)
			return 0;

		/* check if its a device (i.e. console, printer, serial port) */
		if (GetFileType(handle) != FILE_TYPE_CHAR)
			return 0;

		if (!GetConsoleMode(handle, &mode))
			return 0;
	}

	return result;
}
