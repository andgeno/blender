/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 */

#ifndef __BKE_LIGHT_H__
#define __BKE_LIGHT_H__

/** \file
 * \ingroup bke
 * \brief General operations, lookup, etc. for blender lights.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "BLI_compiler_attrs.h"

struct Light;
struct Main;

void BKE_light_init(struct Light *la);
struct Light *BKE_light_add(struct Main *bmain, const char *name) ATTR_WARN_UNUSED_RESULT;
void BKE_light_copy_data(struct Main *bmain,
                         struct Light *la_dst,
                         const struct Light *la_src,
                         const int flag);
struct Light *BKE_light_copy(struct Main *bmain, const struct Light *la) ATTR_WARN_UNUSED_RESULT;
struct Light *BKE_light_localize(struct Light *la) ATTR_WARN_UNUSED_RESULT;
void BKE_light_make_local(struct Main *bmain, struct Light *la, const int flags);
void BKE_light_free(struct Light *la);

#ifdef __cplusplus
}
#endif

#endif
