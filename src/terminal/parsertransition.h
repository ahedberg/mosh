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

#ifndef PARSERTRANSITION_HPP
#define PARSERTRANSITION_HPP

#include <cstdlib>

#include "src/terminal/parseraction.h"

namespace Parser {
  class State;

  class Transition
  {
  public:
    // Transition is only a courier for an Action; it should
    // never create/delete one on its own.
    ActionPointer action;
    State *next_state;

    Transition( const Transition &x )
      : action( x.action ),
	next_state( x.next_state ) {}
    Transition & operator=( const Transition &t )
    {
      action = t.action;
      next_state = t.next_state;

      return *this;
    }
    Transition( ActionPointer s_action = std::make_shared<Ignore>(), State *s_next_state=NULL )
      : action( s_action ), next_state( s_next_state )
    {}

    // This is only ever used in the 1-argument form;
    // we use this instead of an initializer to
    // tell Coverity the object never owns *action.
    Transition( State *s_next_state, ActionPointer s_action = std::make_shared<Ignore>() )
      : action( s_action ), next_state( s_next_state )
    {}
  };
}

#endif
