#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

#include <linux/input-event-codes.h>

#include <compositor-surface.hpp>
#include <output.hpp>
#include <opengl.hpp>
#include <core.hpp>
#include <debug.hpp>
#include <decorator.hpp>
#include <view-transform.hpp>
#include <signal-definitions.hpp>
#include "deco-subsurface.hpp"
#include "deco-layout.hpp"
#include "deco-theme.hpp"
#include "cairo-util.hpp"

#include <cairo.h>

extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_xcursor_manager.h>
#undef static
}

class simple_decoration_surface : public wf::surface_interface_t,
    public wf::compositor_surface_t, public wf_decorator_frame_t
{
    bool _mapped = true;
    int current_thickness;
    int current_titlebar;

    wayfire_view view;

    wf::signal_callback_t title_set = [=] (wf::signal_data_t *data)
    {
        if (get_signaled_view(data) == view)
            view->damage(); // trigger re-render
    };

    void update_title(int width, int height, double scale)
    {
        int target_width = width * scale;
        int target_height = height * scale;

        if (title_texture.width != target_width ||
            title_texture.height != target_height ||
            title_texture.current_text != view->get_title())
        {
            auto surface = theme.render_text(view->get_title(),
                target_width, target_height);
            cairo_surface_upload_to_texture(surface, title_texture.tex);

            title_texture.width = target_width;
            title_texture.height = target_height;
            title_texture.current_text = view->get_title();
        }
    }

    int width = 100, height = 100;

    bool active = true; // when views are mapped, they are usually activated
    struct {
        GLuint tex = -1;
        int width = 0;
        int height = 0;
        std::string current_text = "";
    } title_texture;

    wf::decor::decoration_theme_t theme;
    wf::decor::decoration_layout_t layout;
    wf_region cached_region;

  public:
    simple_decoration_surface(wayfire_view view, wf_option font) :
        surface_interface_t(view.get()),
        theme{},
        layout{theme}
    {
        this->view = view;
        view->connect_signal("title-changed", &title_set);

        // make sure to hide frame if the view is fullscreen
        update_decoration_size();
    }

    virtual ~simple_decoration_surface()
    {
        _mapped = false;
        wf::emit_map_state_change(this);
        view->disconnect_signal("title-changed", &title_set);
    }

    /* wf::surface_interface_t implementation */
    virtual bool is_mapped() const final
    {
        return _mapped;
    }

    wf_point get_offset() final
    {
        return { -current_thickness, -current_titlebar };
    }

    virtual wf_size_t get_size() const final
    {
        return {width, height};
    }

    void render_title(const wf_framebuffer& fb,
        wf_geometry geometry)
    {
        update_title(geometry.width, geometry.height, fb.scale);
        render_gl_texture(fb, geometry, title_texture.tex);
    }

    void render_scissor_box(const wf_framebuffer& fb, wf_point origin,
        const wlr_box& scissor)
    {
        /* Clear background */
        wlr_box geometry {origin.x, origin.y, width, height};
        theme.render_background(fb, geometry, scissor, active);

        /* Draw title & buttons */
        auto renderables = layout.get_renderable_areas();
        for (auto item : renderables)
        {
            if (item->get_type() == wf::decor::DECORATION_AREA_TITLE) {
                OpenGL::render_begin(fb);
                fb.scissor(scissor);
                render_title(fb, item->get_geometry() + origin);
                OpenGL::render_end();
            } else { // button
                item->as_button().render(fb,
                    item->get_geometry() + origin, scissor);
            }
        }
    }

    virtual void simple_render(const wf_framebuffer& fb, int x, int y,
        const wf_region& damage) override
    {
        wf_region frame = this->cached_region + wf_point{x, y};
        frame *= fb.scale;
        frame &= damage;

        for (const auto& box : frame)
        {
            auto sbox = fb.framebuffer_box_from_damage_box(
                wlr_box_from_pixman_box(box));
            render_scissor_box(fb, {x, y}, sbox);
        }
    }

    bool accepts_input(int32_t sx, int32_t sy) override
    {
        return pixman_region32_contains_point(cached_region.to_pixman(),
            sx, sy, NULL);
    }

    /* wf::compositor_surface_t implementation */
    virtual void on_pointer_enter(int x, int y) override
    {
        cursor_x = x;
        cursor_y = y;

        update_cursor();
    }

    virtual void on_pointer_leave() override
    { }

    int cursor_x, cursor_y;
    virtual void on_pointer_motion(int x, int y) override
    {
        cursor_x = x;
        cursor_y = y;

        update_cursor();
    }

    void send_move_request()
    {
        move_request_signal move_request;
        move_request.view = view;
        get_output()->emit_signal("move-request", &move_request);
    }

    void send_resize_request(int x, int y)
    {
        resize_request_signal resize_request;
        resize_request.view = view;
        resize_request.edges = get_edges(x, y);
        get_output()->emit_signal("resize-request", &resize_request);
    }

    uint32_t get_edges(int x, int y)
    {
        uint32_t edges = 0;
        if (x <= current_thickness)
            edges |= WLR_EDGE_LEFT;
        if (x >= width - current_thickness)
            edges |= WLR_EDGE_RIGHT;
        if (y <= current_thickness)
            edges |= WLR_EDGE_TOP;
        if (y >= height - current_thickness)
            edges |= WLR_EDGE_BOTTOM;

        return edges;
    }

    std::string get_cursor(uint32_t edges)
    {
        if (edges)
            return wlr_xcursor_get_resize_name((wlr_edges) edges);
        return "default";
    }

    void update_cursor()
    {
        wf::get_core().set_cursor(get_cursor(get_edges(cursor_x, cursor_y)));
    }

    virtual void on_pointer_button(uint32_t button, uint32_t state) override
    {
        if (button != BTN_LEFT || state != WLR_BUTTON_PRESSED)
            return;

        if (get_edges(cursor_x, cursor_y))
            return send_resize_request(cursor_x, cursor_y);
        send_move_request();
    }

    virtual void on_touch_down(int x, int y) override
    {
        if (get_edges(x, y))
            return send_resize_request(x, y);

        send_move_request();
    }

    /* frame implementation */
    virtual wf_geometry expand_wm_geometry(
        wf_geometry contained_wm_geometry) override
    {
        contained_wm_geometry.x -= current_thickness;
        contained_wm_geometry.y -= current_titlebar;
        contained_wm_geometry.width += 2 * current_thickness;
        contained_wm_geometry.height += current_thickness + current_titlebar;

        return contained_wm_geometry;
    }

    virtual void calculate_resize_size(
        int& target_width, int& target_height) override
    {
        target_width -= 2 * current_thickness;
        target_height -= current_thickness + current_titlebar;

        target_width = std::max(target_width, 1);
        target_height = std::max(target_height, 1);
    }

    virtual void notify_view_activated(bool active) override
    {
        if (this->active != active)
            view->damage();

        this->active = active;
    }

    virtual void notify_view_resized(wf_geometry view_geometry) override
    {
        view->damage();
        width = view_geometry.width;
        height = view_geometry.height;

        layout.resize(width, height);
        this->cached_region = layout.calculate_region();

        view->damage();
    };

    virtual void notify_view_tiled() override
    { }

    void update_decoration_size()
    {
        if (view->fullscreen)
        {
            current_thickness = 0;
            current_titlebar = 0;
        } else
        {
            current_thickness = theme.get_border_size();
            current_titlebar =
                theme.get_title_height() + theme.get_border_size();
        }
        this->cached_region = layout.calculate_region();
    }

    virtual void notify_view_fullscreen() override
    {
        update_decoration_size();
    };
};

void init_view(wayfire_view view, wf_option font)
{
    auto surf = new simple_decoration_surface(view, font);
    view->set_decoration(surf);
    view->damage();
}
