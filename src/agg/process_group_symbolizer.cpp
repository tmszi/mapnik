/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2011 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

// mapnik
#include <mapnik/feature.hpp>
#include <mapnik/agg_renderer.hpp>
#include <mapnik/agg_rasterizer.hpp>
#include <mapnik/agg_render_marker.hpp>
#include <mapnik/image_util.hpp>
#include <mapnik/util/variant.hpp>
#include <mapnik/text/renderer.hpp>
#include <mapnik/geom_util.hpp>
#include <mapnik/symbolizer.hpp>
#include <mapnik/pixel_position.hpp>
#include <mapnik/renderer_common/process_group_symbolizer.hpp>
#include <mapnik/renderer_common/clipping_extent.hpp>
#include <mapnik/svg/svg_renderer_agg.hpp>
#include <mapnik/svg/svg_path_attributes.hpp>
#include <mapnik/svg/svg_path_adapter.hpp>
#include <mapnik/svg/svg_converter.hpp>
// agg
#include "agg_trans_affine.h"

namespace mapnik {

/**
 * Render a thunk which was frozen from a previous call to
 * extract_bboxes. We should now have a new offset at which
 * to render it, and the boxes themselves should already be
 * in the detector from the placement_finder.
 */
struct thunk_renderer : public util::static_visitor<>
{
    using renderer_type = agg_renderer<image_32>;
    using buffer_type = renderer_type::buffer_type;
    using text_renderer_type = agg_text_renderer<buffer_type>;

    thunk_renderer(renderer_type &ren,
                   std::unique_ptr<rasterizer> const& ras_ptr,
                   buffer_type *buf,
                   renderer_common &common,
                   pixel_position const &offset)
        : ren_(ren), ras_ptr_(ras_ptr), buf_(buf), common_(common), offset_(offset)
    {}

    void operator()(vector_marker_render_thunk const &thunk) const
    {
        using blender_type = agg::comp_op_adaptor_rgba_pre<agg::rgba8, agg::order_rgba>; // comp blender
        using buf_type = agg::rendering_buffer;
        using pixfmt_comp_type = agg::pixfmt_custom_blend_rgba<blender_type, buf_type>;
        using renderer_base = agg::renderer_base<pixfmt_comp_type>;
        using renderer_type = agg::renderer_scanline_aa_solid<renderer_base>;
        using svg_attribute_type = agg::pod_bvector<svg::path_attributes>;
        using svg_renderer_type = svg::svg_renderer_agg<svg_path_adapter,
                                                        svg_attribute_type,
                                                        renderer_type,
                                                        pixfmt_comp_type>;
        ras_ptr_->reset();
        buf_type render_buffer(buf_->raw_data(), buf_->width(), buf_->height(), buf_->width() * 4);
        pixfmt_comp_type pixf(render_buffer);
        pixf.comp_op(static_cast<agg::comp_op_e>(thunk.comp_op_));
        renderer_base renb(pixf);
        svg::vertex_stl_adapter<svg::svg_path_storage> stl_storage(thunk.src_->source());
        svg_path_adapter svg_path(stl_storage);
        svg_renderer_type svg_renderer(svg_path, thunk.attrs_);

        agg::trans_affine offset_tr = thunk.tr_;
        offset_tr.translate(offset_.x, offset_.y);
        render_vector_marker(svg_renderer, *ras_ptr_, renb, thunk.src_->bounding_box(), offset_tr, thunk.opacity_, thunk.snap_to_pixels_);
    }

    void operator()(raster_marker_render_thunk const &thunk) const
    {
        using blender_type = agg::comp_op_adaptor_rgba_pre<agg::rgba8, agg::order_rgba>; // comp blender
        using buf_type = agg::rendering_buffer;
        using pixfmt_comp_type = agg::pixfmt_custom_blend_rgba<blender_type, buf_type>;
        using renderer_base = agg::renderer_base<pixfmt_comp_type>;

        ras_ptr_->reset();
        buf_type render_buffer(buf_->raw_data(), buf_->width(), buf_->height(), buf_->width() * 4);
        pixfmt_comp_type pixf(render_buffer);
        pixf.comp_op(static_cast<agg::comp_op_e>(thunk.comp_op_));
        renderer_base renb(pixf);

        agg::trans_affine offset_tr = thunk.tr_;
        offset_tr.translate(offset_.x, offset_.y);
        render_raster_marker(renb, *ras_ptr_, thunk.src_, offset_tr, thunk.opacity_, common_.scale_factor_, thunk.snap_to_pixels_);
    }

    void operator()(text_render_thunk const &thunk) const
    {
        text_renderer_type ren(*buf_, thunk.halo_rasterizer_, thunk.comp_op_, thunk.comp_op_,
                               common_.scale_factor_, common_.font_manager_.get_stroker());

        render_offset_placements(
            thunk.placements_,
            offset_,
            [&] (glyph_positions_ptr glyphs)
            {
                if (glyphs->marker())
                {
                    ren_.render_marker(glyphs->marker_pos(),
                                       *(glyphs->marker()->marker),
                                       glyphs->marker()->transform,
                                       thunk.opacity_, thunk.comp_op_);
                }
                ren.render(*glyphs);
            });
    }

    template <typename T>
    void operator()(T const &) const
    {
        // TODO: warning if unimplemented?
    }

private:
    renderer_type &ren_;
    std::unique_ptr<rasterizer> const& ras_ptr_;
    buffer_type *buf_;
    renderer_common &common_;
    pixel_position offset_;
};

template <typename T0, typename T1>
void agg_renderer<T0,T1>::process(group_symbolizer const& sym,
                                  mapnik::feature_impl & feature,
                                  proj_transform const& prj_trans)
{
    render_group_symbolizer(
        sym, feature, common_.vars_, prj_trans, clipping_extent(common_), common_,
        [&](render_thunk_list const& thunks, pixel_position const& render_offset)
        {
            thunk_renderer ren(*this, ras_ptr, current_buffer_, common_, render_offset);
            for (render_thunk_ptr const& thunk : thunks)
            {
                util::apply_visitor(ren, *thunk);
            }
        });
}

template void agg_renderer<image_32>::process(group_symbolizer const&,
                                              mapnik::feature_impl &,
                                              proj_transform const&);

}
