/*
 * Copyright 2023 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "poly/cl/libpoly.h"
#include "poly/geometry.h"
#include "poly/nir/poly_nir.h"
#include "poly/tessellator.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "nir.h"
#include "nir_builder.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "shader_enums.h"

static nir_def *
tcs_unrolled_id(nir_builder *b)
{
   nir_def *params = nir_load_tess_param_buffer_poly(b);
   nir_def *workgroup_id = nir_load_workgroup_id(b);
   nir_def *patches_per_instance = nir_load_global_constant(
      b, 1, 32,
      nir_iadd_imm(b, params,
                   offsetof(struct poly_tess_params, patches_per_instance)),
      .align_mul = 4);

   return nir_iadd(b,
                   nir_imul(b, nir_channel(b, workgroup_id, 1),
                            patches_per_instance),
                   nir_channel(b, workgroup_id, 0));
}

static nir_def *
tcs_patch_vertices_in(nir_builder *b)
{
   nir_def *params = nir_load_tess_param_buffer_poly(b);

   return nir_load_global_constant(
      b, 1, 32,
      nir_iadd_imm(b, params,
                   offsetof(struct poly_tess_params, input_patch_size)),
      .align_mul = 4);
}

static nir_def *
tes_unrolled_patch_id(nir_builder *b, nir_def *raw_id)
{
   return nir_udiv_imm(b, raw_id, POLY_TES_PATCH_ID_STRIDE);
}

static nir_def *
tes_patch_id(nir_builder *b, nir_def *params, nir_def *raw_id)
{
   nir_def *patch = tes_unrolled_patch_id(b, raw_id);
   nir_def *patches_per_instance = nir_load_global_constant(
      b, 1, 32,
      nir_iadd_imm(b, params,
                   offsetof(struct poly_tess_params, patches_per_instance)),
      .align_mul = 4);

   return nir_umod(b, patch, patches_per_instance);
}

static nir_def *
tes_load_coord(nir_builder *b, nir_def *params, nir_def *raw_id)
{
   nir_def *patch = tes_unrolled_patch_id(b, raw_id);
   nir_def *vertex = nir_umod_imm(b, raw_id, POLY_TES_PATCH_ID_STRIDE);
   nir_def *coord_allocs = nir_load_global_constant(
      b, 1, 64,
      nir_iadd_imm(b, params,
                   offsetof(struct poly_tess_params, coord_allocs)),
      .align_mul = 8);
   nir_def *coord_index = nir_load_global(
      b, 1, 32,
      nir_iadd(b, coord_allocs, nir_ishl_imm(b, nir_u2u64(b, patch), 2)),
      .align_mul = 4);
   nir_def *coord_buffer = nir_load_global_constant(
      b, 1, 64,
      nir_iadd_imm(b, params,
                   offsetof(struct poly_tess_params, patch_coord_buffer)),
      .align_mul = 8);
   nir_def *index = nir_iadd(b, coord_index, vertex);
   nir_def *addr =
      nir_iadd(b, coord_buffer, nir_ishl_imm(b, nir_u2u64(b, index), 3));
   nir_def *fixed =
      nir_load_global_constant(b, 2, 32, addr, .align_mul = 8);

   return nir_fmul(b, nir_u2f32(b, fixed),
                   nir_imm_float(b, 1.0f / 65536.0f));
}

static nir_def *
tes_load_index(nir_builder *b, nir_def *params, nir_def *index)
{
   nir_def *ccw = nir_load_global_constant(
      b, 1, 32,
      nir_iadd_imm(b, params, offsetof(struct poly_tess_params, ccw)),
      .align_mul = 4);
   nir_def *triangle_vertex = nir_umod_imm(b, index, 3);
   nir_def *swapped_index = nir_bcsel(
      b, nir_ieq_imm(b, triangle_vertex, 1), nir_iadd_imm(b, index, 1),
      nir_bcsel(b, nir_ieq_imm(b, triangle_vertex, 2),
                nir_iadd_imm(b, index, -1), index));
   index = nir_bcsel(b, nir_ine_imm(b, ccw, 0), swapped_index, index);

   nir_def *index_buffer = nir_load_global_constant(
      b, 1, 64,
      nir_iadd_imm(b, params, offsetof(struct poly_tess_params, index_buffer)),
      .align_mul = 8);
   nir_def *addr =
      nir_iadd(b, index_buffer, nir_ishl_imm(b, nir_u2u64(b, index), 2));
   return nir_load_global_constant(b, 1, 32, addr, .align_mul = 4);
}

uint64_t
poly_tcs_per_vertex_outputs(const nir_shader *nir)
{
   return nir->info.outputs_written &
          ~(VARYING_BIT_TESS_LEVEL_INNER | VARYING_BIT_TESS_LEVEL_OUTER |
            VARYING_BIT_BOUNDING_BOX0 | VARYING_BIT_BOUNDING_BOX1);
}

unsigned
poly_tcs_output_stride(const nir_shader *nir)
{
   return poly_tcs_out_stride(util_last_bit(nir->info.patch_outputs_written),
                              nir->info.tess.tcs_vertices_out,
                              poly_tcs_per_vertex_outputs(nir));
}

static nir_def *
tcs_output_address(nir_builder *b, nir_def *params, nir_def *patch_id,
                   nir_def *vertex_id, nir_def *location,
                   nir_def *nr_patch_out, nir_def *out_patch_size,
                   nir_def *vtx_out_mask, unsigned component)
{
   nir_def *location_mask = nir_iadd_imm(
      b, nir_ishl(b, nir_imm_int64(b, 1), nir_u2u64(b, location)), -1);
   nir_def *vtx_out_count = nir_u2u32(b, nir_bit_count(b, vtx_out_mask));
   nir_def *nr_patch_out_bytes = nir_imul_imm(b, nr_patch_out, 4);
   nir_def *vertex_offset = nir_iadd_imm(
      b, nir_imul(b, vertex_id, nir_imul_imm(b, vtx_out_count, 4)), 6);
   vertex_offset = nir_iadd(b, vertex_offset, nr_patch_out_bytes);
   vertex_offset = nir_iadd(
      b, vertex_offset,
      nir_imul_imm(
         b, nir_u2u32(b, nir_bit_count(
                            b, nir_iand(b, vtx_out_mask, location_mask))),
         4));
   nir_def *patch_offset = nir_iadd_imm(
      b,
      nir_ishl_imm(
         b, nir_iadd_imm(b, location, -VARYING_SLOT_PATCH0), 2),
      6);
   nir_def *offset_el = nir_bcsel(
      b, nir_uge_imm(b, location, VARYING_SLOT_PATCH0), patch_offset,
      vertex_offset);
   offset_el = nir_bcsel(b, nir_ieq_imm(b, location, VARYING_SLOT_TESS_LEVEL_INNER),
                         nir_imm_int(b, 4), offset_el);
   offset_el = nir_bcsel(b, nir_ieq_imm(b, location, VARYING_SLOT_TESS_LEVEL_OUTER),
                         nir_imm_int(b, 0), offset_el);
   nir_def *stride_el = nir_iadd_imm(
      b,
      nir_imul_imm(
         b, nir_imul(b, out_patch_size, vtx_out_count), 4),
      6);
   stride_el = nir_iadd(b, stride_el, nr_patch_out_bytes);
   offset_el =
      nir_iadd(b, offset_el, nir_imul(b, patch_id, stride_el));

   nir_def *tcs_buffer = nir_load_global_constant(
      b, 1, 64,
      nir_iadd_imm(b, params, offsetof(struct poly_tess_params, tcs_buffer)),
      .align_mul = 8);
   nir_def *addr = nir_iadd(
      b, tcs_buffer, nir_ishl_imm(b, nir_u2u64(b, offset_el), 2));

   return nir_iadd_imm(b, addr, component * 4);
}

static nir_def *
tcs_out_addr(nir_builder *b, nir_intrinsic_instr *intr, nir_def *vertex_id)
{
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   nir_def *offset = nir_get_io_offset_src(intr)->ssa;
   uint32_t nr_patch_out =
      util_last_bit(b->shader->info.patch_outputs_written);
   uint64_t vtx_out_mask = poly_tcs_per_vertex_outputs(b->shader);

   return tcs_output_address(
      b, nir_load_tess_param_buffer_poly(b), tcs_unrolled_id(b), vertex_id,
      nir_iadd_imm(b, offset, sem.location), nir_imm_int(b, nr_patch_out),
      nir_imm_int(b, b->shader->info.tess.tcs_vertices_out),
      nir_imm_int64(b, vtx_out_mask), nir_intrinsic_component(intr));
}

static nir_def *
tcs_tess_level_default(nir_builder *b, bool outer)
{
   nir_def *params = nir_load_tess_param_buffer_poly(b);
   size_t offset = outer ? offsetof(struct poly_tess_params,
                                    tess_level_outer_default)
                         : offsetof(struct poly_tess_params,
                                    tess_level_inner_default);
   unsigned components = outer ? 4 : 2;

   return nir_load_global_constant(
      b, components, 32, nir_iadd_imm(b, params, offset), .align_mul = 4);
}

static nir_def *
lower_tes_load(nir_builder *b, nir_intrinsic_instr *intr)
{
   gl_varying_slot location = nir_intrinsic_io_semantics(intr).location;
   nir_src *offset_src = nir_get_io_offset_src(intr);

   nir_def *vertex = nir_imm_int(b, 0);
   nir_def *offset = offset_src ? offset_src->ssa : nir_imm_int(b, 0);

   if (intr->intrinsic == nir_intrinsic_load_per_vertex_input)
      vertex = intr->src[0].ssa;

   nir_def *params = nir_load_tess_param_buffer_poly(b);
   nir_def *nr_patch_out = nir_load_global_constant(
      b, 1, 32,
      nir_iadd_imm(b, params,
                   offsetof(struct poly_tess_params, tcs_patch_constants)),
      .align_mul = 4);
   nir_def *out_patch_size = nir_load_global_constant(
      b, 1, 32,
      nir_iadd_imm(b, params,
                   offsetof(struct poly_tess_params, output_patch_size)),
      .align_mul = 4);
   nir_def *vtx_out_mask = nir_load_global_constant(
      b, 1, 64,
      nir_iadd_imm(b, params,
                   offsetof(struct poly_tess_params, tcs_per_vertex_outputs)),
      .align_mul = 8);
   nir_def *patch_id = nir_udiv_imm(
      b, nir_load_vertex_id(b), POLY_TES_PATCH_ID_STRIDE);
   nir_def *addr = tcs_output_address(
      b, params, patch_id, vertex, nir_iadd_imm(b, offset, location),
      nr_patch_out, out_patch_size, vtx_out_mask,
      nir_intrinsic_has_component(intr) ? nir_intrinsic_component(intr) : 0);

   return nir_load_global_constant(b, intr->def.num_components,
                                   intr->def.bit_size, addr, .align_mul = 4);
}

static nir_def *
tcs_load_input(nir_builder *b, nir_intrinsic_instr *intr)
{
   nir_def *base = nir_imul(b, tcs_unrolled_id(b), tcs_patch_vertices_in(b));
   nir_def *vertex = nir_iadd(b, base, intr->src[0].ssa);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   nir_def *location = nir_iadd_imm(b, intr->src[1].ssa, sem.location);
   nir_def *vp = nir_load_vertex_param_buffer_poly(b);
   nir_def *output_mask = nir_load_vs_outputs_poly(b);
   nir_def *location_mask = nir_iadd_imm(
      b, nir_ishl(b, nir_imm_int64(b, 1), nir_u2u64(b, location)), -1);
   nir_def *offset_el = nir_iadd(
      b, nir_imul(b, vertex, nir_u2u32(b, nir_bit_count(b, output_mask))),
      nir_u2u32(b, nir_bit_count(b, nir_iand(b, output_mask, location_mask))));
   nir_def *output_buffer = nir_load_global_constant(
      b, 1, 64,
      nir_iadd_imm(b, vp, offsetof(struct poly_vertex_params, output_buffer)),
      .align_mul = 8);
   nir_def *addr =
      nir_iadd(b, output_buffer, nir_ishl_imm(b, nir_u2u64(b, offset_el), 4));
   addr = nir_iadd_imm(b, addr, 4 * nir_intrinsic_component(intr));

   return nir_load_global_constant(b, intr->def.num_components,
                                   intr->def.bit_size, addr, .align_mul = 4);
}

static nir_def *
lower_tcs_impl(nir_builder *b, nir_intrinsic_instr *intr,
               bool can_ignore_shader_out_barriers)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_barrier: {
      /* We lowered the TCS outputs from shader_out to global memory,
       * preserve barriers if they don't target shader outputs or modify their
       * execution/memory scopes and modes accordingly to match subgroups since
       * a patch fits in a subgroup. */
      nir_variable_mode modes = nir_intrinsic_memory_modes(intr);

      /* Skip barriers that don't handle shader outputs */
      if (!(modes & nir_var_shader_out))
         return NULL;

      modes &= ~nir_var_shader_out;

      /* A barrier that only targeted shader outputs can be dropped entirely by
       * drivers that don't need it. */
      if (can_ignore_shader_out_barriers && !modes)
         return NIR_LOWER_INSTR_PROGRESS_REPLACE;

      /* Keep the original scopes while the barrier still targets other modes;
       * otherwise a subgroup scope suffices since a patch fits in a subgroup.
       * Drivers that can't ignore the shader-output barrier must preserve it
       * against the lowered global memory. */
      mesa_scope execution_scope =
         modes ? nir_intrinsic_execution_scope(intr) : SCOPE_SUBGROUP;
      mesa_scope memory_scope =
         modes ? nir_intrinsic_memory_scope(intr) : SCOPE_SUBGROUP;
      if (!can_ignore_shader_out_barriers)
         modes |= nir_var_mem_global;

      nir_barrier(b, .execution_scope = execution_scope,
                  .memory_scope = memory_scope,
                  .memory_semantics = nir_intrinsic_memory_semantics(intr),
                  .memory_modes = modes);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   case nir_intrinsic_load_primitive_id:
      return nir_channel(b, nir_load_workgroup_id(b), 0);

   case nir_intrinsic_load_instance_id:
      return nir_channel(b, nir_load_workgroup_id(b), 1);

   case nir_intrinsic_load_invocation_id:
      if (b->shader->info.tess.tcs_vertices_out == 1)
         return nir_imm_int(b, 0);
      else
         return nir_channel(b, nir_load_local_invocation_id(b), 0);

   case nir_intrinsic_load_per_vertex_input:
      return tcs_load_input(b, intr);

   case nir_intrinsic_load_patch_vertices_in:
      return tcs_patch_vertices_in(b);

   case nir_intrinsic_load_tess_level_outer_default:
      return tcs_tess_level_default(b, true);

   case nir_intrinsic_load_tess_level_inner_default:
      return tcs_tess_level_default(b, false);

   case nir_intrinsic_load_output: {
      nir_def *addr = tcs_out_addr(b, intr, nir_undef(b, 1, 32));
      return nir_load_global(b, intr->def.num_components, intr->def.bit_size,
                             addr, .align_mul = 4);
   }

   case nir_intrinsic_load_per_vertex_output: {
      nir_def *addr = tcs_out_addr(b, intr, intr->src[0].ssa);
      return nir_load_global(b, intr->def.num_components, intr->def.bit_size,
                             addr, .align_mul = 4);
   }

   case nir_intrinsic_store_output: {
      /* Only vec2, make sure we can't overwrite */
      assert(intr->src[0].ssa->num_components <= 2 ||
             nir_intrinsic_io_semantics(intr).location !=
                VARYING_SLOT_TESS_LEVEL_INNER);

      nir_store_global(b, intr->src[0].ssa,
                       tcs_out_addr(b, intr, nir_undef(b, 1, 32)),
                       .write_mask = nir_intrinsic_write_mask(intr));
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   case nir_intrinsic_store_per_vertex_output: {
      nir_store_global(b, intr->src[0].ssa,
                       tcs_out_addr(b, intr, intr->src[1].ssa),
                       .write_mask = nir_intrinsic_write_mask(intr));
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   default:
      return NULL;
   }
}

static bool
lower_tcs(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   b->cursor = nir_before_instr(&intr->instr);

   nir_def *repl = lower_tcs_impl(b, intr, !!(uintptr_t)data);
   if (!repl)
      return false;

   if (repl != NIR_LOWER_INSTR_PROGRESS_REPLACE)
      nir_def_rewrite_uses(&intr->def, repl);

   nir_instr_remove(&intr->instr);
   return true;
}

bool
poly_nir_lower_tcs(nir_shader *tcs, bool can_ignore_shader_out_barriers)
{
   return nir_shader_intrinsics_pass(
      tcs, lower_tcs, nir_metadata_control_flow,
      (void *)(uintptr_t)can_ignore_shader_out_barriers);
}

static nir_def *
lower_tes_impl(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_tess_coord_xy:
      return tes_load_coord(b, nir_load_tess_param_buffer_poly(b),
                            nir_load_vertex_id(b));

   case nir_intrinsic_load_primitive_id:
      return tes_patch_id(b, nir_load_tess_param_buffer_poly(b),
                          nir_load_vertex_id(b));

   case nir_intrinsic_load_input:
   case nir_intrinsic_load_per_vertex_input:
   case nir_intrinsic_load_tess_level_inner:
   case nir_intrinsic_load_tess_level_outer:
      return lower_tes_load(b, intr);

   case nir_intrinsic_load_patch_vertices_in:
      return nir_load_global_constant(
         b, 1, 32,
         nir_iadd_imm(b, nir_load_tess_param_buffer_poly(b),
                      offsetof(struct poly_tess_params, output_patch_size)),
         .align_mul = 4);

   default:
      return NULL;
   }
}

static bool
lower_tes(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *repl = lower_tes_impl(b, intr, data);

   if (repl) {
      nir_def_replace(&intr->def, repl);
      return true;
   } else {
      return false;
   }
}

static bool
lower_tes_indexing(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_vertex_id)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *p = nir_load_tess_param_buffer_poly(b);
   nir_def *id = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def_replace(&intr->def, tes_load_index(b, p, id));
   return true;
}

bool
poly_nir_lower_tes(nir_shader *tes, bool to_hw_vs)
{
   nir_lower_tess_coord_z(
      tes, tes->info.tess._primitive_mode == TESS_PRIMITIVE_TRIANGLES);

   nir_shader_intrinsics_pass(tes, lower_tes, nir_metadata_control_flow, NULL);

   /* Points mode renders as points, make sure we write point size for the HW */
   if (tes->info.tess.point_mode && to_hw_vs) {
      nir_lower_default_point_size(tes);
   }

   if (to_hw_vs) {
      /* We lower to a HW VS, so update the shader info so the compiler does the
       * right thing.
       */
      tes->info.stage = MESA_SHADER_VERTEX;
      memset(&tes->info.vs, 0, sizeof(tes->info.vs));
      tes->info.vs.tes_poly = true;
   } else {
      /* If we're running as a compute shader, we need to load from the index
       * buffer manually. Fortunately, this doesn't require a shader key:
       * tess-as-compute always use U32 index buffers.
       */
      nir_shader_intrinsics_pass(tes, lower_tes_indexing,
                                 nir_metadata_control_flow, NULL);
   }

   nir_lower_idiv(tes, &(nir_lower_idiv_options){.allow_fp16 = true});
   return nir_progress(true, nir_shader_get_entrypoint(tes), nir_metadata_none);
}
