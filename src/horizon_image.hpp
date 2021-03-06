#ifndef HORIZON_IMAGE_HPP
#define HORIZON_IMAGE_HPP

#include <gtkmm/container.h>
#include <gtkmm/eventbox.h>
#include <gtkmm/image.h>
#include <gdkmm/pixbuf.h>
#include <gtkmm/grid.h>
#include "image_fetcher.hpp"
#include "thread.hpp"
#include "canceller.hpp"
#include <sigc++/functors/slot.h>

namespace Horizon {

	class Image : public Gtk::Bin {
	public:
		enum ImageState { NONE, THUMBNAIL, EXPAND, FULL };
		virtual ~Image();

		Glib::RefPtr<Gdk::Pixbuf> get_image() const;

		void set_state(const ImageState new_state);

		Image(const Glib::RefPtr<Post>&,
		      const std::shared_ptr<ImageFetcher>& image_fetcher,
		      sigc::slot<void, const Image::ImageState&>);

	protected:

		virtual Gtk::SizeRequestMode get_request_mode_vfunc() const;
		virtual void get_preferred_width_vfunc(int& minimum_width, int& natural_width) const;
		virtual void get_preferred_height_for_width_vfunc(int width, int& minimum_height, int& natural_height) const;
		virtual void get_preferred_height_vfunc(int& minimum_height, int& natural_height) const;
		virtual void get_preferred_width_for_height_vfunc(int height, int& minimum_width, int& natural_width) const;
		virtual void on_size_allocate(Gtk::Allocation& allocation);
		
	private:
		Glib::RefPtr<Post>                     post;
		Gtk::EventBox                         *event_box;
		Gtk::Image                            *image;
		void on_area_prepared(Glib::RefPtr<Gdk::Pixbuf> pixbuf);
		void on_area_updated(int, int, int, int);

		ImageState                             image_state;
		sigc::signal<void, const ImageState&>  signal_state_changed;
		bool                                   is_changing_state;
		Glib::Dispatcher                       state_changed_dispatcher;
		void run_state_changed_callbacks() const;

		Glib::RefPtr<Gdk::Pixbuf>              thumbnail_image;
		Glib::RefPtr<Gdk::Pixbuf>              unscaled_image;
		Glib::RefPtr<Gdk::Pixbuf>              scaled_image;
		Glib::RefPtr<Gdk::PixbufAnimation>     unscaled_animation;
		Glib::RefPtr<Gdk::PixbufAnimationIter> animation_iter;
		Glib::TimeVal                          animation_time;
		bool on_animation_timeout();
		int                                    scaled_width;
		int                                    scaled_height;
		bool                                   is_scaled;
		bool                                   am_fetching_thumb;
		bool                                   am_fetching_image;
		
		std::shared_ptr<Canceller>             canceller;

		sigc::connection                       animation_timeout;
		void on_image_unmap();
		void on_image_unrealize();
		bool on_image_draw(const Cairo::RefPtr< Cairo::Context > &cr);
		void reset_animation_iter();

		void fetch_thumbnail();
		void fetch_image();
		void on_thumb(const Glib::RefPtr<Gdk::PixbufLoader> &loader);
		void on_image(const Glib::RefPtr<Gdk::PixbufLoader> &loader);
		void set_none_state();
		void set_thumb_state();
		void set_expand_state();
		std::weak_ptr<ImageFetcher>          ifetcher;

		void refresh_size_request();
		void set_new_scaled_image(const int width, const int height);
		bool on_image_click(GdkEventButton* btn);
		
	};

}

#endif
