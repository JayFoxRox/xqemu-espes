/*
 * QEMU Geforce NV2A shader common definitions
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_NV2A_SHADERS_COMMON_H
#define HW_NV2A_SHADERS_COMMON_H

#define STRUCT_VERTEX_DATA "struct VertexData {\n" \
                           "  float inv_w;\n" \
                           "  vec4 D0;\n" \
                           "  vec4 D1;\n" \
                           "  vec4 B0;\n" \
                           "  vec4 B1;\n" \
                           "  vec4 Fog;\n" \
                           "  vec4 T0;\n" \
                           "  vec4 T1;\n" \
                           "  vec4 T2;\n" \
                           "  vec4 T3;\n" \
                           "};\n"

#define VERTEX_TRANSFORM "/* Clip range */" \
"  if (false && clipRange.y != clipRange.x) {\n" \
"    gl_ClipDistance[0] = (oPos.z + clipRange.x);\n" \
"    gl_ClipDistance[1] = (-oPos.z + clipRange.y);\n" \
"  } else {" \
"    gl_ClipDistance[0] = 1.0;\n" \
"    gl_ClipDistance[1] = 1.0;\n" \
"  }\n" \
\
/* the shaders leave the result in screen space, while
* opengl expects it in clip space.
* TODO: the pixel-center co-ordinate differences should handled
*/ \
"oPos.xyz = 2.0 * oPos.xyz / surfaceSize - 1.0;\n" \
"oPos.y *= -1.0;\n" \
/* HONESTLY: NO IDEA WHAT  I DO HERE, FOUND BY MESSING AROUND.. */ \
/* Z was [0,size] ???. GL needs [-1,+1].
 * First move to [-0.5,+0.5] then [-1.0,0.0]
 */ \
"oPos.z *= 0.5; oPos.z -= 0.5;" \
/*
"if (clipRange.y != clipRange.x) {\n"
"  oPos.z = (oPos.z - 0.5 * (clipRange.x + clipRange.y)) / (0.5 * (clipRange.y - clipRange.x));\n"
"}\n"
*/ \
    /* Correct for the perspective divide */ \
    "if (oPos.w <= 0.0) {\n" \
    /* undo the perspective divide in the case where the point would be
    * clipped so opengl can clip it correctly */ \
    "  oPos.xyz *= oPos.w;\n" \
    "} else {\n" \
    /* we don't want the OpenGL perspective divide to happen, but we
    * can't multiply by W because it could be meaningless here */ \
    "  oPos.w = 1.0;\n" \
    "}\n"


#endif
