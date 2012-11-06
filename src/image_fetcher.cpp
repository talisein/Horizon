#include "image_fetcher.hpp"
#include <iostream>
#include <glibmm/fileutils.h>
#include "utils.hpp"

#include "horizon_curl.cpp"

namespace Horizon {
	std::shared_ptr<ImageFetcher> ImageFetcher::get(FETCH_TYPE type) {
		static std::shared_ptr<ImageFetcher> singleton_4chan;
		static std::shared_ptr<ImageFetcher> singleton_catalog;
		
		if (!singleton_4chan) {
			singleton_4chan = std::shared_ptr<ImageFetcher>(new ImageFetcher());
		}

		if (!singleton_catalog) {
			singleton_catalog = std::shared_ptr<ImageFetcher>(new ImageFetcher());
		}
		
		switch (type) {
		case FOURCHAN:
			return singleton_4chan;
		case CATALOG:
			return singleton_catalog;
		default:
			return singleton_4chan;
		}
	}
	
	const Glib::RefPtr<Gdk::Pixbuf> ImageFetcher::get_thumb(const std::string &hash) const {
		Glib::Mutex::Lock lock(thumbs_mutex);
		
		auto iter = thumbs.find(hash);
		if ( iter != thumbs.end() ) {
			return iter->second;
		} else {
			g_error("ImageFetcher::get_thumb called for an image not yet downloaded");
			return Glib::RefPtr<Gdk::Pixbuf>();
		}
	}

	const Glib::RefPtr<Gdk::Pixbuf> ImageFetcher::get_image(const std::string &hash) const {
		Glib::Mutex::Lock lock(images_mutex);
		
		auto iter = images.find(hash);
		if ( iter != images.end() ) {
			return iter->second;
		} else {
			g_error("ImageFetcher::get_image called for an image not yet downloaded");
			return Glib::RefPtr<Gdk::Pixbuf>();
		}
	}

	const Glib::RefPtr<Gdk::PixbufAnimation> ImageFetcher::get_animation(const std::string &hash) const {
		Glib::Mutex::Lock lock(images_mutex);
		
		auto iter = animations.find(hash);
		if ( iter != animations.end() ) {
			return iter->second;
		} else {
			g_error("ImageFetcher::get_animation called for an image not yet downloaded");
			return Glib::RefPtr<Gdk::PixbufAnimation>();
		}
	}

	bool ImageFetcher::has_thumb(const std::string &hash) const {
		Glib::Mutex::Lock lock(thumbs_mutex);
		return thumbs.count(hash) > 0;
	}

	bool ImageFetcher::has_image(const std::string &hash) const {
		Glib::Mutex::Lock lock(images_mutex);
		return images.count(hash) > 0;
	}

	bool ImageFetcher::has_animation(const std::string &hash) const {
		Glib::Mutex::Lock lock(images_mutex);
		return animations.count(hash) > 0;
	}

	std::size_t ImageFetcher::curl_writeback(const std::string &str_buf,
	                                         std::shared_ptr<Request> request) {
		std::size_t wrote = 0;
		std::string hash = request->hash;
		Glib::RefPtr< Gdk::PixbufLoader > loader = request->loader;
		Glib::RefPtr< Gio::MemoryInputStream > istream = request->istream;
		
		if (G_LIKELY( loader && istream )) {
			try {
				loader->write(reinterpret_cast<const guint8*>(str_buf.c_str()),
				              static_cast<gsize>(str_buf.size()));
				wrote = str_buf.size();
				gpointer buf = g_memdup(str_buf.c_str(), str_buf.size());
				istream->add_data(buf, str_buf.size(), &g_free);
			} catch (Gdk::PixbufError e) {
				std::cerr << "Error writing pixbuf from network: " << e.what() << std::endl;
			} catch (Glib::FileError e) {
				std::cerr << "Error writing pixbuf from network: " << e.what() << std::endl;
			}
		} else {
			g_critical("Got an invalid loader or stream");
			return wrote;
		}

		return wrote;
	}

	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::start_new_download() {
		std::vector< std::pair<std::shared_ptr<CurlEasy<std::shared_ptr<Request> > >,
		                       std::shared_ptr<Request> > > work_list;
		{ /* Old Interface */
			Glib::Mutex::Lock lock(curl_data_mutex);
			while ( !curl_queue.empty() ) {
				if (request_queue.empty()) {
					break;
				} else {
					std::shared_ptr<CurlEasy<std::shared_ptr<Request> > > easy = curl_queue.front();
					curl_queue.pop();
					std::shared_ptr<Request> request = request_queue.front();
					request_queue.pop();
					work_list.push_back(std::make_pair(easy, request));
				}
			}
		} /* Old Interface */

		{ /* New Interface */
			Glib::Threads::RWLock::WriterLock wlock(request_queue_rwlock);
			Glib::Mutex::Lock lock(curl_data_mutex);
			while ( (!curl_queue.empty()) && (!new_request_queue.empty()) ) {
					std::shared_ptr<CurlEasy<std::shared_ptr<Request> > > easy = curl_queue.front();
					curl_queue.pop();
					std::shared_ptr<Request> request = new_request_queue.front();
					new_request_queue.pop_front();
					work_list.push_back(std::make_pair(easy, request));
			}
		} /* New Interface */

		for (auto pair : work_list) {
			std::shared_ptr<CurlEasy<std::shared_ptr<Request> > > easy = pair.first;
			std::shared_ptr<Request> request = pair.second;
				
			if (request) {
				const std::string url  = request->url;

				easy->reset();
				auto write_functor = std::bind(&ImageFetcher::curl_writeback,
				                               this,
				                               std::placeholders::_1,
				                               request);
				easy->set_write_function(write_functor);
				easy->set_url(url);
				easy->set_private(request);
				easy->set_fail_on_error();
				easy->set_error_buffer(curl_error_buffer);
				easy->set_connect_timeout(3);
				easy->set_no_signal();
			
				try {
					// Supports "jpeg" "gif" "png"
					request->loader = Gdk::PixbufLoader::create();
					auto area_prepared_slot = sigc::bind(sigc::mem_fun(*this, &ImageFetcher::on_area_prepared),
					                                     request);
					request->area_prepared_connection = request->loader->signal_area_prepared().connect(area_prepared_slot);

					auto area_updated_slot = sigc::bind(sigc::mem_fun(*this, &ImageFetcher::on_area_updated),
					                                    request);
					request->area_updated_connection = request->loader->signal_area_updated().connect(area_updated_slot);

					curl_multi->add_handle(easy);
				} catch ( Gdk::PixbufError e ) {
					std::cerr << "Download aborted for url " << url << std::endl;
					Glib::Mutex::Lock lock(curl_data_mutex);
					curl_queue.push(easy);
					queue_w.send();
				}

			} else {
				Glib::Mutex::Lock lock(curl_data_mutex);
				curl_queue.push(easy);
				queue_w.send();
			}
		} // for work_list
	}
	
	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::on_area_prepared(std::shared_ptr<Request> request) {
		if (request->area_prepared_functor) {
			auto pixbuf = request->loader->get_pixbuf();
			auto functor = std::bind(request->area_prepared_functor, pixbuf);
			
			{
				Glib::Mutex::Lock lock(pixbuf_mutex);
				pixbuf_update_queue.push_back(functor);
			}

			signal_pixbuf_updated();
		}

		request->area_prepared_connection.disconnect();
	}

	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::on_area_updated(int x, int y, int width, int height,
	                                   std::shared_ptr<Request> request) {
		if (request->area_updated_functor) {
			auto functor = std::bind(request->area_updated_functor, x, y, width, height);
			{
				Glib::Mutex::Lock lock(pixbuf_mutex);
				pixbuf_update_queue.push_back(functor);
			}
			signal_pixbuf_updated();
		}
	}

	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::signal_pixbuf_updated() {
		Glib::Threads::Mutex::Lock lock(pixbuf_updated_idle_mutex);

		if (!pixbuf_updated_idle.connected()) {
			pixbuf_updated_idle = Glib::signal_idle().connect(sigc::mem_fun(*this, &ImageFetcher::on_pixbuf_updated));
		}
	}

	/*
	 * Called on Glib::Main thread idle
	 */
	bool ImageFetcher::on_pixbuf_updated() {
		Glib::Threads::Mutex::Lock lock(pixbuf_updated_idle_mutex);
		bool ret = true;
		std::function<void ()> functor;
		{
			Glib::Mutex::Lock lock(pixbuf_mutex);
			if (pixbuf_update_queue.size() > 0) {
				functor = pixbuf_update_queue.front();
				functor();
				pixbuf_update_queue.pop_front();
			}

			if ( pixbuf_update_queue.size() == 0 ) {
				pixbuf_updated_idle.disconnect();
				ret = false;
			} else {
				ret = true;
			}
		}

		return ret;
	}

	/*
	 * Called from Glib main thread
	 */
	void ImageFetcher::download_thumb(const Glib::RefPtr<Post> &post) {
		//const std::string &hash, const std::string &url) {
		bool is_new = false;

		{
			Glib::Mutex::Lock lock(thumbs_mutex);
			auto pair = pending_thumbs.insert(post->get_hash());
			is_new = pair.second;
		}
		
		if (is_new) {
			std::shared_ptr<Request> req(new Request());
			req->hash = post->get_hash();
			req->url = post->get_thumb_url();
			req->is_thumb = true;
			req->ext = ".jpg";
			req->istream = Gio::MemoryInputStream::create();
			req->post = post;

			if (image_cache->has_thumb(post)) {
				auto functor = std::bind(&ImageFetcher::on_cache_result,
				                         this,
				                         std::placeholders::_1,
				                         req);
				image_cache->get_thumb_async(post, functor);
			} else {
				{
					Glib::Mutex::Lock lock(curl_data_mutex);
					request_queue.push(req);
				}
				queue_w.send();
			}
		}
	}

	/*
	 * Called from Glib main thread
	 */
	void ImageFetcher::download_image(const Glib::RefPtr<Post> &post,
	                                  std::function<void (Glib::RefPtr<Gdk::Pixbuf>)> area_prepared,
	                                  std::function<void (int, int, int, int)> area_updated) {
		bool is_new = false;

		{
			Glib::Mutex::Lock lock(images_mutex);
			auto pair = pending_images.insert(post->get_hash());
			is_new = pair.second;
		}

		if (is_new) {
			std::shared_ptr<Request> req(new Request());
			req->hash = post->get_hash();
			req->url = post->get_image_url();
			req->is_thumb = false;
			req->ext = post->get_image_ext();
			req->istream = Gio::MemoryInputStream::create();
			req->post = post;
			req->area_prepared_functor = area_prepared;
			req->area_updated_functor = area_updated;

			if (image_cache->has_image(post)) {
				auto functor = std::bind(&ImageFetcher::on_cache_result,
				                         this,
				                         std::placeholders::_1,
				                         req);
				image_cache->get_image_async(post, functor);
			} else {
				{
					Glib::Mutex::Lock lock(curl_data_mutex);
					request_queue.push(req);
				}
				queue_w.send();
			}
		}
	}

	static std::string get_request_key(const Glib::RefPtr<Post> &post, bool get_thumb) {
		std::string hash;
		if (get_thumb)
			hash = "T_";
		else
			hash = "I_";
		hash.append(post->get_hash());

		return hash;
	}
	
	static std::shared_ptr<Request> create_request(const Glib::RefPtr<Post> &post,
	                                            bool is_thumb,
	                                            std::function<void (Glib::RefPtr<Gdk::Pixbuf>)> area_prepared_cb,
	                                            std::function<void (int, int, int, int)> area_updated_cb) {
		std::shared_ptr<Request> req(new Request());
		req->hash = post->get_hash();
		req->is_thumb = is_thumb;
		if (is_thumb) {
			req->url = post->get_thumb_url();
			req->ext = ".jpg";
		} else {
			req->url = post->get_image_url();
			req->ext = post->get_image_ext();
		}
		req->istream = Gio::MemoryInputStream::create();
		req->post = post;
		req->area_prepared_functor = area_prepared_cb;
		req->area_updated_functor = area_updated_cb;

		return req;
	}

	/*
	 * Called from Glib main thread
	 */
	void ImageFetcher::download(const Glib::RefPtr<Post> &post,
	                            std::function<void (const Glib::RefPtr<Gdk::PixbufLoader> &loader)> callback,
	                            bool get_thumb,
	                            std::function<void (Glib::RefPtr<Gdk::Pixbuf>)> area_prepared_cb,
	                            std::function<void (int, int, int, int)> area_updated_cb) {
		std::string request_key = get_request_key(post, get_thumb);

		bool is_pending = add_request_cb(request_key, callback);
		if ( !is_pending ) {
			bool in_cache = false;
			std::shared_ptr<Request> request = create_request(post, get_thumb,
			                                                  area_prepared_cb,
			                                                  area_updated_cb);
			auto cache_cb = std::bind(&ImageFetcher::on_cache_result, this,
			                          std::placeholders::_1,
			                          request);
			if (get_thumb) {
				if (image_cache->has_thumb(post)) {
					image_cache->get_thumb_async(post, cache_cb);
					in_cache = true;
				}
			} else {
				if (image_cache->has_image(post)) {
					image_cache->get_image_async(post, cache_cb);
					in_cache = true;
				}
			}

			if (!in_cache) {
				add_request(request);
			}
		}
	}

	/*
	 * Called from Glib main thread
	 */
	bool ImageFetcher::add_request_cb(const std::string &request_key,
	                                  std::function<void (const Glib::RefPtr<Gdk::PixbufLoader> &)> cb) {
		bool is_pending = false;
		Glib::Threads::RWLock::WriterLock lock(request_cb_rwlock);

		auto iter = request_cb_map.find(request_key);
		if ( iter != request_cb_map.end() )
			is_pending = true;
		
		request_cb_map.insert(iter, std::make_pair(request_key, cb));

		return is_pending;
	}

	/*
	 * Called from Glib main thread
	 */
	void ImageFetcher::add_request(const std::shared_ptr<Request> &request) {
		Glib::Threads::RWLock::WriterLock lock(request_queue_rwlock);

		new_request_queue.push_back(request);

		queue_w.send();
	}

	/*
	 * Called on ev_thread
	 */
	int curl_socket_cb(CURL *easy,      /* easy handle */
	                   curl_socket_t s, /* socket */
	                   int action,      /* see values below */
	                   void *userp,     /* private callback pointer */
	                   void *socketp)   /* private socket pointer */
	{
		Socket_Info* info = static_cast<Socket_Info*>(socketp);
		ImageFetcher *ifetcher = static_cast<ImageFetcher*>(userp);
		
		if (action == CURL_POLL_REMOVE) {
			ifetcher->curl_remsock(info, s);
		} else {
			if (!info) {
				ifetcher->curl_addsock(s, easy, action);
			} else {
				ifetcher->curl_setsock(info, s, easy, action);
			}
		}
		return 0;
	}

	/*
	 * Called on ev_thread
	 */
	int curl_timer_cb(CURLM *, long timeout_ms, void *userp) {
		ImageFetcher *ifetcher = static_cast<ImageFetcher*>(userp);

		if (ifetcher->timeout_w.is_active())
			ifetcher->timeout_w.stop();

		if (timeout_ms == -1) {
		} else if (timeout_ms == 0) {
			ifetcher->curl_timeout_expired_cb();
		} else {
			ev_tstamp SEC_PER_MS = 0.001;
			ev_tstamp timeout = static_cast<ev_tstamp>(timeout_ms) * SEC_PER_MS;
			ifetcher->timeout_w.set( timeout );
			ifetcher->timeout_w.start();
		}

		return 0;
	}

	/*
	 * Called on ev_thread
	 */
	bool ImageFetcher::curl_timeout_expired_cb() {
		int old_handles = running_handles;
		curl_multi->socket_action_timeout(&running_handles);

		if ( running_handles < old_handles)
			curl_check_info();
		return false;
	}

	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::curl_event_cb(ev::io &w, int) {
		curl_socket_t s = static_cast<curl_socket_t>(w.fd);
		int old_handles = running_handles;

		curl_multi->socket_action(s, 0, &running_handles);

		if ( running_handles < old_handles)
			curl_check_info();
		if (running_handles == 0) {
			if (timeout_w.is_active())
				timeout_w.stop();
		}
	}
	
	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::curl_setsock(Socket_Info* info, curl_socket_t s,
	                                CURL*, int action) {
		int condition = 0;

		if (action & CURL_POLL_IN)
			condition = ev::READ;
		if (action & CURL_POLL_OUT)
			condition = condition | ev::WRITE;
		if (action & CURL_POLL_INOUT)
			condition = condition | ev::WRITE | ev::READ;

		if (info->w)
			info->w.reset();
		info->w = std::shared_ptr<ev::io>(new ev::io(ev_loop));
		info->w->set<ImageFetcher, &ImageFetcher::curl_event_cb>(this);
		info->w->set(static_cast<int>(s), condition);
		info->w->start();
	}

	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::curl_addsock(curl_socket_t s, CURL *easy, int action) {
		if( G_LIKELY( std::count(active_sockets_.begin(),
		                         active_sockets_.end(), s) == 0 )) {
			active_sockets_.push_back(s);
		} else {
			g_critical("Error: curl_addsock got called twice for the same socket. "
			           "I don't know what to do!");
		}

		Socket_Info* info;
		if (socket_info_cache_.empty()) {
			info = new Socket_Info();
		} else {
			info = socket_info_cache_.back();
			socket_info_cache_.pop_back();
		}

		if ( G_LIKELY(std::count(active_socket_infos_.begin(),
		                         active_socket_infos_.end(), info) == 0 )) {
			active_socket_infos_.push_back(info);
		} else {
			g_critical("Error: Somehow the info we are assigning is already active!");
		}

		curl_setsock(info, s, easy, action);
		curl_multi->multi_assign(s, info);
	}

	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::curl_remsock(Socket_Info* info, curl_socket_t s) {
		if (G_UNLIKELY( !info ))
			return;

		if (G_LIKELY( std::count(active_sockets_.begin(),
		                         active_sockets_.end(), s) > 0 )) {
			if (G_LIKELY( std::count(active_socket_infos_.begin(),
			                         active_socket_infos_.end(), info) > 0 )) {
				// We SHOULD NOT call info->channel->close(). This closes curl's
				// cached connection to the server.
				info->w->stop();
				socket_info_cache_.push_back(info);
				active_socket_infos_.remove(info);
				active_sockets_.remove(s);
			} else {
				g_critical("curl_remsock called on inactive info.");
			}
		} else {
			g_warning("curl_remsock was called twice for the same socket.");
		}
	}

	void ImageFetcher::remove_pending(std::shared_ptr<Request> request) {
		if (request->is_thumb) {
			Glib::Mutex::Lock lock(thumbs_mutex);
			auto iter = pending_thumbs.find(request->hash);
			if (iter != pending_thumbs.end()) {
				pending_thumbs.erase(iter);
			} else {
				g_warning("A failed download couldn't be found pending");
			}
		} else {
			Glib::Mutex::Lock lock(images_mutex);
			auto iter = pending_images.find(request->hash);
			if (iter != pending_images.end()) {
				pending_images.erase(iter);
			} else {
				g_warning("A failed download couldn't be found pending");
			}
		}
	}

	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::cleanup_failed_pixmap(std::shared_ptr<Request> request,
	                                         bool is_404) {
		Glib::RefPtr<Gdk::PixbufLoader> loader = request->loader;
		bool use_old_interface = false;

		if (G_LIKELY( loader )) {
			try {
				loader->close();
			} catch (Gdk::PixbufError e) {
				// This is expected since this was a failed download
			} catch (Glib::FileError e) {
			}
		}

		/*
		 * TODO: for !404 errors, put the requests in a cooldown queue
		 */
		loader.reset();
		const std::string request_key = get_request_key(request->post,
		                                                request->is_thumb);
		bool found_cb = bind_loader_to_callbacks(request_key, loader);
		use_old_interface = !found_cb;

		if (use_old_interface) { /* Old Interface */
			if (is_404) {
				GError *error = nullptr;
				const char* resource_404 = "/com/talisein/fourchan/native/gtk/404-Anonymous-2.png";
				GdkPixbuf *cpixbuf_404 = gdk_pixbuf_new_from_resource(resource_404,
				                                                      &error);
				if (G_LIKELY( !error )) {
					auto pixbuf_404 = Glib::wrap(cpixbuf_404);
					{
						Glib::Mutex::Lock lock(pixbuf_mutex);
						pixbuf_map.insert({request, pixbuf_404});
					}
					signal_pixbuf_ready();
				} else {
					g_warning("Unable to create 404 image from resource: %s",
					          error->message);
				}
			}
		} /* Old Interface */

		request->area_updated_connection.disconnect();
	}

	/*
	 * Called on ev_thread
	 */
	void ImageFetcher::create_pixmap(std::shared_ptr<Request> request) {
		const bool isThumb = request->is_thumb;
		const std::string hash = request->hash;
		const bool isGif = request->ext.rfind("gif") != std::string::npos;
		Glib::RefPtr<Gdk::PixbufLoader> loader = request->loader;
		bool close_error = false;
		bool get_error = false;
		bool use_old_interface = false;

		if ( G_LIKELY( loader ) ) {
			try {
				loader->close();
			} catch (Gdk::PixbufError e) {
				std::cerr << "Error: Closing PixbufLoader for url " << request->url
				          << ": " << e.what() << std::endl;
				close_error = true;
			} catch (Glib::FileError e) {
				std::cerr << "Error: Closing PixbufLoader for url " << request->url
				          << ": " << e.what() << std::endl;
				close_error = true;
				// TODO: Determine which errors we can recover
				// from by trying again
			}
		} else {
			g_error( "Loader doesn't have a valid pointer." );
			return;
		}

		if (close_error) {
			loader.reset();
			request->istream->close();
		} else {
			if (isThumb) {
				image_cache->write_thumb_async(request->post,
				                               request->istream);
			} else {
				image_cache->write_image_async(request->post,
				                               request->istream);
			}
		}

		const std::string request_key = get_request_key(request->post,
		                                                request->is_thumb);
		bool found_cb = bind_loader_to_callbacks(request_key, loader);
		use_old_interface = !found_cb;


		if (use_old_interface) { /* Old Interface */
			if (G_LIKELY( !close_error && loader )) {
				if (isGif) {
					Glib::RefPtr<Gdk::PixbufAnimation> pixbuf_animation;
					try {
						pixbuf_animation = loader->get_animation();
					} catch (Gdk::PixbufError e) {
						std::cerr << "Unexpected Pixbuf error fetching "
						          << "from loader for url " 
						          << request->url << ": "
						          << e.what() << std::endl;
						get_error = true;
					}

					if (!get_error && pixbuf_animation) {
						{
							Glib::Mutex::Lock lock(pixbuf_mutex);
							pixbuf_animation_map.insert({request, pixbuf_animation});
						}
						signal_pixbuf_ready();
					}
				} else {
					Glib::RefPtr<Gdk::Pixbuf> pixbuf;
					try {
						pixbuf = loader->get_pixbuf();
					} catch (Gdk::PixbufError e) {
						std::cerr << "Unexpected Pixbuf error fetching "
						          << "from loader for url " 
						          << request->url << ": "
						          << e.what() << std::endl;
						get_error = true;
					}
				
					if (!get_error && pixbuf) {
						{
							Glib::Mutex::Lock lock(pixbuf_mutex);
							pixbuf_map.insert({request, pixbuf});
						}
						signal_pixbuf_ready();
					}
				}
			}
		} /* Old interface */

		request->area_updated_connection.disconnect();
		
	}

	/*
	 * Called on ImageCache thread or ev_thread
	 */
	bool ImageFetcher::bind_loader_to_callbacks(const std::string &request_key,
	                                            const Glib::RefPtr<Gdk::PixbufLoader> &loader) {
		bool found_cb = false;
		Glib::Threads::RWLock::WriterLock lock(request_cb_rwlock);
		auto iter_pair = request_cb_map.equal_range(request_key);
		auto lower_bound = iter_pair.first;
		auto upper_bound = iter_pair.second;
		if (lower_bound != request_cb_map.end()) {
			found_cb = true;
			Glib::Threads::RWLock::WriterLock cb_queue_lock(cb_queue_rwlock);
			std::transform(lower_bound,
			               upper_bound,
			               std::back_inserter(cb_queue),
			               [&loader](const std::pair<std::string,
			                         std::function<void (const Glib::RefPtr<Gdk::PixbufLoader> &)>
			                         > &pair) {
				               return std::bind(pair.second, loader);
			               });
			request_cb_map.erase(lower_bound, upper_bound);
			if (!cb_queue_idle.connected()) {
				cb_queue_idle =  Glib::signal_idle().connect(sigc::mem_fun(*this, &ImageFetcher::process_cb_queue),
				                                             Glib::PRIORITY_DEFAULT);
			}
		}

		return found_cb;
	}

	/*
	 * Called on Glib MainLoop idle
	 */
	bool ImageFetcher::process_cb_queue() {
		bool ret = true;
		Glib::Threads::RWLock::WriterLock cb_queue_lock(cb_queue_rwlock);
		if ( !cb_queue.empty() ) {
			auto cb = cb_queue.front();
			cb();
			cb_queue.pop_front();
		}

		if ( cb_queue.empty() ) {
			ret = false;
			cb_queue_idle.disconnect();
		}

		return ret;
	}

	/* 
	 * Called from ImageCache thread
	 */
	void ImageFetcher::on_cache_result(const Glib::RefPtr<Gdk::PixbufLoader>& loader,
	                                   std::shared_ptr<Request> request) {
		bool load_error = false;

		if (loader) {
			const std::string request_key = get_request_key(request->post,
			                                                request->is_thumb);
			bool use_old_interface = false;

			bool found_cb = bind_loader_to_callbacks(request_key, loader);
			use_old_interface = !found_cb;

			if (use_old_interface) {
				/* Old Interface */
				if ( request->ext.find(".gif") != request->ext.npos ) {
					Glib::RefPtr<Gdk::PixbufAnimation> ani_pixbuf;
					try {
						ani_pixbuf = loader->get_animation();
					} catch (Gdk::PixbufError e) {
						load_error = true;
						g_warning("Error loading from pixbuf: %s", e.what().c_str());
					}

					if (ani_pixbuf && !load_error) {
						{
							Glib::Mutex::Lock lock(pixbuf_mutex);
							pixbuf_animation_map.insert({request, ani_pixbuf});
						}
						signal_pixbuf_ready();
					}
				} else {
					Glib::RefPtr<Gdk::Pixbuf> pixbuf;
					try {
						pixbuf = loader->get_pixbuf();
					} catch (Gdk::PixbufError e) {
						load_error = true;
						g_warning("Error loading from pixbuf: %s", e.what().c_str());
					}
				
					if (pixbuf && !load_error) {
						{
							Glib::Mutex::Lock lock(pixbuf_mutex);
							pixbuf_map.insert({request, pixbuf});
						}
						signal_pixbuf_ready();
					}
				}
				/* Old Interface */
			}
		} else {
			// There was an error from disk, so fallback to downloading.
			std::cerr << "Info: An on disk image was invalid or corrupt, so we are downloading it again." << std::endl;
			add_request(request);
		}

	}

	/*
	 * Called on the GLib Main Thread
	 */
	void ImageFetcher::on_pixbuf_ready() {
		std::pair<std::shared_ptr<Request>, Glib::RefPtr<Gdk::Pixbuf> > pixbuf_pair;
		bool had_pixbuf = false;
		{
			Glib::Mutex::Lock lock(pixbuf_mutex);
			if (pixbuf_map.size() > 0) {
				auto iter = pixbuf_map.begin();
				pixbuf_pair = *iter;
				pixbuf_map.erase(iter);
				had_pixbuf = true;
			}
		}
				
		if (had_pixbuf) {
			auto request = pixbuf_pair.first;
			auto pixbuf = pixbuf_pair.second;
			if (request->is_thumb) {
				{
					Glib::Mutex::Lock lock(thumbs_mutex);
					thumbs.insert({request->hash, pixbuf});
				}
				signal_thumb_ready(request->hash);
			} else {
				{
					Glib::Mutex::Lock lock(images_mutex);
					images.insert({request->hash, pixbuf});
				}
				signal_image_ready(request->hash);
			}
			remove_pending(request);
		}


		std::pair<std::shared_ptr<Request>, Glib::RefPtr<Gdk::PixbufAnimation> > animation_pair;
		bool had_animation = false;
		{
			Glib::Mutex::Lock lock(pixbuf_mutex);
			if ( pixbuf_animation_map.size() > 0 ) {
				auto iter = pixbuf_animation_map.begin();
				animation_pair = *iter;
				pixbuf_animation_map.erase(iter);
				had_animation = true;
			}
		}
		
		if (had_animation) {
			auto request = animation_pair.first;
			auto pixbuf = animation_pair.second;
			{
				Glib::Mutex::Lock lock(images_mutex);
				animations.insert({request->hash, pixbuf});
			}
			signal_image_ready(request->hash);
			remove_pending(request);
		}

		bool should_signal = false;
		{
			Glib::Mutex::Lock lock(pixbuf_mutex);
			should_signal = (pixbuf_map.size() > 0 
			                 || pixbuf_animation_map.size() > 0);
		}

		if (should_signal)
			signal_pixbuf_ready();
	}


	/* 
	 * Called on ev_thread
	 *
	 * Lock already held here
	 */
	void ImageFetcher::curl_check_info() {
		int msgs_left;
		CURLcode res;
		bool download_error = false;
		bool download_error_404 = false;
		std::shared_ptr<Request> request;
		std::shared_ptr<CurlEasy<std::shared_ptr<Request> > > easy;
		do { 
			auto pair = curl_multi->info_read(&msgs_left);
			easy = pair.first;
			res = pair.second;
			if (easy) {
				request = easy->get_private();

				if ( G_UNLIKELY(res != CURLE_OK) ) {
					download_error = true;
					long code = easy->get_response_code();
					if (res  == CURLE_HTTP_RETURNED_ERROR &&
					    code == 404) {
						download_error_404 = true;
					} else {
						std::cerr << "Error: Downloading " << request->url
						          << " : " << curl_error_buffer << std::endl;
					}
				}

				curl_multi->remove_handle(easy);

				if (G_LIKELY(!download_error)) {
					create_pixmap(request);
				} else {
					cleanup_failed_pixmap(request, download_error_404);
				}

				{
					Glib::Mutex::Lock lock(curl_data_mutex);
					curl_queue.push(easy);
				}
				queue_w.send();
			}
		} while(easy && msgs_left > 0);
	}

	void ImageFetcher::on_kill_loop_w(ev::async &, int) {
		kill_loop_w.stop();
		queue_w.stop();
	}

	void ImageFetcher::on_timeout_w(ev::timer &, int) {
		curl_timeout_expired_cb();
	}

	void ImageFetcher::on_queue_w(ev::async &, int) {
		start_new_download();
	}

	ImageFetcher::ImageFetcher() :
		image_cache(ImageCache::get()),
		curl_error_buffer(g_new0(char, CURL_ERROR_SIZE)),
		curl_multi(CurlMulti<std::shared_ptr<Request> >::create()),
		running_handles(0),
		ev_thread(nullptr),
		ev_loop(ev::AUTO),
		kill_loop_w(ev_loop),
		queue_w(ev_loop),
		timeout_w(ev_loop)
	{
		Glib::Mutex::Lock lock(curl_data_mutex);
		signal_pixbuf_ready.connect( sigc::mem_fun(*this, &ImageFetcher::on_pixbuf_ready) );

		curl_multi->set_socket_function(&curl_socket_cb, this);
		curl_multi->set_timer_function(&curl_timer_cb, this);

		for (int i = 0; i < 3; i++) {
			auto easy = CurlEasy<std::shared_ptr<Request> >::create();
			curl_queue.push(easy);
		}

		queue_w.set<ImageFetcher, &ImageFetcher::on_queue_w> (this);
		queue_w.start();

		kill_loop_w.set<ImageFetcher, &ImageFetcher::on_kill_loop_w> (this);
		kill_loop_w.start();

		timeout_w.set<ImageFetcher, &ImageFetcher::on_timeout_w> (this);

		ev_loop.set_io_collect_interval(0.1);

		const sigc::slot<void> slot = sigc::bind(sigc::mem_fun(ev_loop, &ev::dynamic_loop::run),0);
		int trycount = 0;

		while ( ev_thread == nullptr && trycount++ < 10 ) {
			try {
				ev_thread = Horizon::create_named_thread("ImageFetcher",
				                                         slot);
			} catch ( Glib::Threads::ThreadError e) {
				if (e.code() != Glib::Threads::ThreadError::AGAIN) {
					g_error("Couldn't create ImageFetcher thread: %s",
					        e.what().c_str());
				}
			}
		}
		
		if (!ev_thread)
			g_error("Couldn't spin up ImageFetcher thread");
	}

	ImageFetcher::~ImageFetcher() {
		kill_loop_w.send();
		ev_thread->join();

		g_free(curl_error_buffer);
	}
}
