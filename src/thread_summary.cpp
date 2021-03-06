#include "thread_summary.hpp"
#include "image_fetcher.hpp"

namespace Horizon {

	const Glib::Class& ThreadSummary_Class::init() {
		if (!gtype_) {
			class_init_func_ = &ThreadSummary_Class::class_init_function;
			register_derived_type(horizon_thread_summary_get_type());
		}
		
		return *this;
	}

	void ThreadSummary_Class::class_init_function(void *g_class, void *class_data) {
		BaseClassType *const klass = static_cast<BaseClassType*>(g_class);
		CppClassParent::class_init_function(klass, class_data);
	}

	Glib::ObjectBase* ThreadSummary_Class::wrap_new(GObject *object) {
		return new ThreadSummary((HorizonThreadSummary*) object);
	}

	HorizonThreadSummary* ThreadSummary::gobj_copy() {
		reference();
		return gobj();
	}

	ThreadSummary::ThreadSummary(HorizonThreadSummary* castitem) :
		Glib::Object((GObject*) castitem),
		canceller(new Canceller())
	{}

	ThreadSummary::ThreadSummary(const Glib::ConstructParams &params) :
		Glib::Object(params),
		canceller(new Canceller())
	{}

	ThreadSummary::CppClassType ThreadSummary::thread_summary_class_;

	GType ThreadSummary::get_type() {
		return thread_summary_class_.init().get_type();
	}

	GType ThreadSummary::get_base_type() {
		return horizon_thread_summary_get_type();
	}

	ThreadSummary::~ThreadSummary() {
		canceller->cancel();
	}

	gint64 ThreadSummary::get_id() const {
		return horizon_thread_summary_get_id(gobj());
	}

	gint64 ThreadSummary::get_unix_date() const {
		return horizon_thread_summary_get_unix_date(gobj());
	}

	void ThreadSummary::set_id(const gint64 id) {
		horizon_thread_summary_set_id(gobj(), id);
	}

	void ThreadSummary::set_id(const gchar *sid) {
		horizon_thread_summary_set_id_from_string(gobj(), sid);
	}

	void ThreadSummary::set_board(const gchar *board) {
		horizon_thread_summary_set_board(gobj(), board);
	}

	const std::string ThreadSummary::get_url() const {
		const gchar *url = horizon_thread_summary_get_url(gobj());
		std::stringstream s;

		if (url)
			s << static_cast<const char *>(url);

		return s.str();
	}

	const std::string ThreadSummary::get_teaser() const {
		std::stringstream s;
		s << horizon_thread_summary_get_teaser(gobj());
		return s.str();
	}

	gint64 ThreadSummary::get_image_count() const {
		return horizon_thread_summary_get_image_count(gobj());
	}

	gint64 ThreadSummary::get_reply_count() const {
		return horizon_thread_summary_get_reply_count(gobj());
	}

	const std::string ThreadSummary::get_hash() const {
		std::stringstream s;
		s << "{FAKEHASH}" 
		  << horizon_thread_summary_get_board(gobj())
		  << "-" << horizon_thread_summary_get_id(gobj()) 
		  << "-" << horizon_thread_summary_get_unix_date(gobj());

		return s.str();
	}

	Glib::RefPtr<Gdk::Pixbuf> ThreadSummary::get_thumb_pixbuf() {
		return Glib::wrap(horizon_thread_summary_get_thumb_pixbuf(gobj()), true);
	}

	Glib::RefPtr<Horizon::Post> ThreadSummary::get_proxy_post() const {
		HorizonPost *cpost = HORIZON_POST(g_object_new(horizon_post_get_type(),
		                                               "md5",
		                                               get_hash().c_str(),
		                                               NULL));
		horizon_post_set_thumb_url(cpost, horizon_thread_summary_get_thumb_url(gobj()));
		auto post = Glib::wrap(cpost);
		return post;
	}

	void ThreadSummary::on_thumb(const Glib::RefPtr<Gdk::PixbufLoader> &loader) {
		if (loader) {
			auto pixbuf = loader->get_pixbuf();
			horizon_thread_summary_set_thumb_pixbuf(gobj(), pixbuf->gobj());
		}
	}
}

namespace Glib {
	Glib::RefPtr<Horizon::ThreadSummary> wrap(HorizonThreadSummary *object, bool take_copy) {
		return Glib::RefPtr<Horizon::ThreadSummary>(dynamic_cast<Horizon::ThreadSummary*>( Glib::wrap_auto ((GObject*)object, take_copy)) );
	}
}
