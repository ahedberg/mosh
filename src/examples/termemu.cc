/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include "src/include/config.h"

#include <cassert>
#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <typeinfo>

#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#if HAVE_PTY_H
#include <pty.h>
#elif HAVE_UTIL_H
#include <util.h>
#elif HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#include "src/terminal/parser.h"
#include "src/statesync/completeterminal.h"
#include "src/util/swrite.h"
#include "src/util/fatal_assert.h"
#include "src/util/pty_compat.h"
#include "src/util/locale_utils.h"
#include "src/util/select.h"

const size_t buf_size = 16384;

static void emulate_terminal( int fd );

int main( int argc, char *argv[] )
{
  int master;
  struct termios saved_termios, raw_termios, child_termios;

  set_native_locale();
  fatal_assert( is_utf8_locale() );

  if ( tcgetattr( STDIN_FILENO, &saved_termios ) < 0 ) {
    perror( "tcgetattr" );
    exit( 1 );
  }

  child_termios = saved_termios;

#ifdef HAVE_IUTF8
  if ( !(child_termios.c_iflag & IUTF8) ) {
    fprintf( stderr, "Warning: Locale is UTF-8 but termios IUTF8 flag not set. Setting IUTF8 flag.\n" );
    child_termios.c_iflag |= IUTF8;
  }
#else
  fprintf( stderr, "Warning: termios IUTF8 flag not defined. Character-erase of multibyte character sequence probably does not work properly on this platform.\n" );
#endif /* HAVE_IUTF8 */

  pid_t child = forkpty( &master, NULL, &child_termios, NULL );

  if ( child == -1 ) {
    perror( "forkpty" );
    exit( 1 );
  }

  if ( child == 0 ) {
    /* child */
    if ( setenv( "TERM", "xterm-256color", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    /* ask ncurses to send UTF-8 instead of ISO 2022 for line-drawing chars */
    if ( setenv( "NCURSES_NO_UTF8_ACS", "1", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    char *my_argv[ 2 ];

    if ( argc > 1 ) {
      argv++;
    } else {
      /* get shell name */
      my_argv[ 0 ] = getenv( "SHELL" );
      if ( my_argv[ 0 ] == NULL || *my_argv[ 0 ] == '\0' ) {
	struct passwd *pw = getpwuid( getuid() );
	if ( pw == NULL ) {
	  perror( "getpwuid" );
	  exit( 1 );
	}
	my_argv[ 0 ] = strdup( pw->pw_shell );
      }
      assert( my_argv[ 0 ] );
      my_argv[ 1 ] = NULL;
      argv = my_argv;
    }
    if ( execvp( argv[ 0 ], argv ) < 0 ) {
      perror( "execve" );
      exit( 1 );
    }
    exit( 0 );
  } else {
    /* parent */
    raw_termios = saved_termios;

    cfmakeraw( &raw_termios );

    if ( tcsetattr( STDIN_FILENO, TCSANOW, &raw_termios ) < 0 ) {
      perror( "tcsetattr" );
      exit( 1 );
    }

    try {
      emulate_terminal( master );
    } catch ( const std::exception &e ) {
      fprintf( stderr, "\r\nException caught: %s\r\n", e.what() );
    }

    if ( tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios ) < 0 ) {
      perror( "tcsetattr" );
      exit( 1 );
    }
  }

  printf( "[stm is exiting.]\n" );

  return 0;
}

/* Print a frame if the last frame was more than 1/50 seconds ago */
static bool tick( Terminal::Framebuffer &state, Terminal::Framebuffer &new_frame,
		  const Terminal::Display &display )
{
  static bool initialized = false;
  static struct timeval last_time;

  struct timeval this_time;

  if ( gettimeofday( &this_time, NULL ) < 0 ) {
    perror( "gettimeofday" );
  }

  double diff = (this_time.tv_sec - last_time.tv_sec)
    + .000001 * (this_time.tv_usec - last_time.tv_usec);

  if ( (!initialized)
       || (diff >= 0.02) ) {
    std::string update = display.new_frame( initialized, state, new_frame );
    swrite( STDOUT_FILENO, update.c_str() );
    state = new_frame;

    initialized = true;
    last_time = this_time;

    return true;
  }

  return false;
}

/* This is the main loop.

   1. New bytes from the user get applied to the terminal emulator
      as "UserByte" actions.

   2. New bytes from the host get sent to the Parser, and then
      those actions are applied to the terminal.

   3. Resize events (from a SIGWINCH signal) get turned into
      "Resize" actions and applied to the terminal.

   At every event from select(), we run the tick() function to
   possibly print a new frame (if we haven't printed one in the
   last 1/50 second). The new frames are "differential" -- they
   assume the previous frame was sent to the real terminal.
*/

static void emulate_terminal( int fd )
{
  /* get current window size */
  struct winsize window_size;
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCGWINSZ" );
    return;
  }

  /* tell child process */
  if ( ioctl( fd, TIOCSWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCSWINSZ" );
    return;
  }

  /* open parser and terminal */
  Terminal::Complete complete( window_size.ws_col, window_size.ws_row );
  Terminal::Framebuffer state( window_size.ws_col, window_size.ws_row );

  /* open display */
  Terminal::Display display( true ); /* use TERM to initialize */

  Select &sel = Select::get_instance();
  sel.add_fd( STDIN_FILENO );
  sel.add_fd( fd );
  sel.add_signal( SIGWINCH );

  swrite( STDOUT_FILENO, display.open().c_str() );

  int timeout = -1;

  while ( 1 ) {
    int active_fds = sel.select( timeout );
    if ( active_fds < 0 ) {
      perror( "select" );
      break;
    }

    if ( sel.read( STDIN_FILENO ) ) {
      /* input from user */
      char buf[ buf_size ];

      /* fill buffer if possible */
      ssize_t bytes_read = read( STDIN_FILENO, buf, buf_size );
      if ( bytes_read == 0 ) { /* EOF */
	return;
      } else if ( bytes_read < 0 ) {
	perror( "read" );
	return;
      }
      
      std::string terminal_to_host;
      
      for ( int i = 0; i < bytes_read; i++ ) {
	terminal_to_host += complete.act( Parser::UserByte( buf[ i ] ) );
      }
      
      if ( swrite( fd, terminal_to_host.c_str(), terminal_to_host.length() ) < 0 ) {
	break;
      }
    } else if ( sel.read( fd ) ) {
      /* input from host */
      char buf[ buf_size ];

      /* fill buffer if possible */
      ssize_t bytes_read = read( fd, buf, buf_size );
      if ( bytes_read == 0 ) { /* EOF */
	return;
      } else if ( bytes_read < 0 ) {
	perror( "read" );
	return;
      }
      
      std::string terminal_to_host = complete.act( std::string( buf, bytes_read ) );
      if ( swrite( fd, terminal_to_host.c_str(), terminal_to_host.length() ) < 0 ) {
	break;
      }
    } else if ( sel.signal( SIGWINCH ) ) {
      /* get new size */
      if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
	perror( "ioctl TIOCGWINSZ" );
	return;
      }

      /* tell emulator */
      complete.act( Parser::Resize( window_size.ws_col, window_size.ws_row ) );

      /* tell child process */
      if ( ioctl( fd, TIOCSWINSZ, &window_size ) < 0 ) {
	perror( "ioctl TIOCSWINSZ" );
	return;
      }
    }

    Terminal::Framebuffer new_frame( complete.get_fb() );

    if ( tick( state, new_frame, display ) ) { /* there was a frame */
      timeout = -1;
    } else {
      timeout = 20;
    }
  }

  std::string update = display.new_frame( true, state, complete.get_fb() );
  swrite( STDOUT_FILENO, update.c_str() );

  swrite( STDOUT_FILENO, display.close().c_str() );
}
