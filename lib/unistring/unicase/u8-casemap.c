/* Case mapping for UTF-8 strings (locale dependent).
   Copyright (C) 2009-2016 Free Software Foundation, Inc.
   Written by Bruno Haible <bruno@clisp.org>, 2009.

   This program is free software: you can redistribute it and/or modify it
   under the terms of either:

    * the GNU Lesser General Public License as published
   by the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   or

   * the GNU General Public License as published by the Free
   Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   or both in parallel, as here.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>

/* Specification.  */
#include "unicasemap.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include "unistr.h"
#include "unictype.h"
#include "uninorm.h"
#include "caseprop.h"
#include "context.h"
#include "special-casing.h"

#define FUNC u8_casemap
#define UNIT uint8_t
#define U_MBTOUC_UNSAFE u8_mbtouc_unsafe
#define U_UCTOMB u8_uctomb
#define U_CPY u8_cpy
#define U_NORMALIZE u8_normalize
#include "u-casemap.h"
