/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 * The Original Code is Copyright (C) Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/modifiers/intern/MOD_boolean_util.h
 *  \ingroup modifiers
 */


#ifndef __MOD_BOOLEAN_UTIL_H__
#define __MOD_BOOLEAN_UTIL_H__

struct Object;
struct DerivedMesh;

/* Performs a boolean between two mesh objects, it is assumed that both objects
 * are in fact mesh object. On success returns a DerivedMesh. On failure
 * returns NULL and reports an error. */

struct DerivedMesh *NewBooleanDerivedMesh(struct DerivedMesh *dm, struct Object *ob,
                                          struct DerivedMesh *dm_select, struct Object *ob_select, int int_op_type);

#endif  /* MOD_BOOLEAN_UTILS */
