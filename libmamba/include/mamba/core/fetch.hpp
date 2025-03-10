// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#ifndef MAMBA_CORE_FETCH_HPP
#define MAMBA_CORE_FETCH_HPP

extern "C"
{
#include <archive.h>
#include <curl/curl.h>
}

#include <string>
#include <vector>

#include <bzlib.h>
#include <zstd.h>

#include "progress_bar.hpp"
#include "validate.hpp"

namespace mamba
{
    void init_curl_ssl();

    struct ZstdStream
    {
        static constexpr size_t BUFFER_SIZE = 256000;
        ZstdStream(curl_write_callback write_callback, void* write_callback_data)
            : stream(ZSTD_createDCtx())
            , m_write_callback(write_callback)
            , m_write_callback_data(write_callback_data)
        {
            ZSTD_initDStream(stream);
        }

        ~ZstdStream()
        {
            ZSTD_freeDCtx(stream);
        }

        size_t write(char* in, size_t size);

        static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* self)
        {
            return static_cast<ZstdStream*>(self)->write(ptr, size * nmemb);
        }

        ZSTD_DCtx* stream;
        char buffer[BUFFER_SIZE];

        // original curl callback
        curl_write_callback m_write_callback;
        void* m_write_callback_data;
    };

    struct Bzip2Stream
    {
        static constexpr size_t BUFFER_SIZE = 256000;

        Bzip2Stream(curl_write_callback write_callback, void* write_callback_data)
            : m_write_callback(write_callback)
            , m_write_callback_data(write_callback_data)
        {
            stream.bzalloc = nullptr;
            stream.bzfree = nullptr;
            stream.opaque = nullptr;

            error = BZ2_bzDecompressInit(&stream, 0, false);
            if (error != BZ_OK)
            {
                throw std::runtime_error("BZ2_bzDecompressInit failed");
            }
        }

        size_t write(char* in, size_t size);

        static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* self)
        {
            return static_cast<Bzip2Stream*>(self)->write(ptr, size * nmemb);
        }

        ~Bzip2Stream()
        {
            BZ2_bzDecompressEnd(&stream);
        }

        int error;
        bz_stream stream;
        char buffer[BUFFER_SIZE];

        // original curl callback
        curl_write_callback m_write_callback;
        void* m_write_callback_data;
    };


    class DownloadTarget
    {
    public:

        DownloadTarget() = default;
        DownloadTarget(const std::string& name, const std::string& url, const std::string& filename);
        ~DownloadTarget();

        DownloadTarget(const DownloadTarget&) = delete;
        DownloadTarget& operator=(const DownloadTarget&) = delete;

        DownloadTarget(DownloadTarget&&);
        DownloadTarget& operator=(DownloadTarget&&);

        static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* self);
        static size_t header_callback(char* buffer, size_t size, size_t nitems, void* self);

        static int progress_callback(
            void*,
            curl_off_t total_to_download,
            curl_off_t now_downloaded,
            curl_off_t,
            curl_off_t
        );
        void set_mod_etag_headers(const std::string& mod, const std::string& etag);
        void set_progress_bar(ProgressProxy progress_proxy);
        void set_expected_size(std::size_t size);
        void set_head_only(bool yes)
        {
            curl_easy_setopt(m_handle, CURLOPT_NOBODY, yes);
        }

        const std::string& name() const;
        const std::string& url() const;
        std::size_t expected_size() const;

        void init_curl_target(const std::string& url);

        bool resource_exists();
        bool perform();
        CURL* handle();

        curl_off_t get_speed();

        template <class C>
        inline void set_finalize_callback(bool (C::*cb)(const DownloadTarget&), C* data)
        {
            m_finalize_callback = std::bind(cb, data, std::placeholders::_1);
        }

        inline void set_ignore_failure(bool yes)
        {
            m_ignore_failure = yes;
        }

        inline bool ignore_failure() const
        {
            return m_ignore_failure;
        }

        void set_result(CURLcode r);
        bool finalize();
        std::string get_transfer_msg();

        bool can_retry();
        CURL* retry();

        std::chrono::steady_clock::time_point progress_throttle_time() const;
        void set_progress_throttle_time(const std::chrono::steady_clock::time_point& time);

        CURLcode result;
        bool failed = false;
        int http_status = 10000;
        char* effective_url = nullptr;
        curl_off_t downloaded_size = 0;
        curl_off_t avg_speed = 0;
        std::string final_url;

        std::string etag, mod, cache_control;

    private:

        std::unique_ptr<ZstdStream> m_zstd_stream;
        std::unique_ptr<Bzip2Stream> m_bzip2_stream;
        std::function<bool(const DownloadTarget&)> m_finalize_callback;

        std::string m_name, m_filename, m_url;

        // validation
        std::size_t m_expected_size = 0;

        // retry & backoff
        std::chrono::steady_clock::time_point m_next_retry;
        std::size_t m_retry_wait_seconds = get_default_retry_timeout();
        std::size_t m_retries = 0;

        CURL* m_handle;
        curl_slist* m_headers;

        bool m_has_progress_bar = false;
        bool m_ignore_failure = false;

        ProgressProxy m_progress_bar;

        char m_errbuf[CURL_ERROR_SIZE];
        std::ofstream m_file;

        static std::size_t get_default_retry_timeout();
        static void init_curl_handle(CURL* handle, const std::string& url);
        std::function<void(ProgressBarRepr&)> download_repr();

        std::chrono::steady_clock::time_point m_progress_throttle_time;
    };

    class MultiDownloadTarget
    {
    public:

        MultiDownloadTarget();
        ~MultiDownloadTarget();

        void add(DownloadTarget* target);
        bool check_msgs(bool failfast);
        bool download(int options);

    private:

        std::vector<DownloadTarget*> m_targets;
        std::vector<DownloadTarget*> m_retry_targets;
        CURLM* m_handle;
    };

    const int MAMBA_DOWNLOAD_FAILFAST = 1 << 0;
    const int MAMBA_DOWNLOAD_SORT = 1 << 1;
    const int MAMBA_NO_CLEAR_PROGRESS_BARS = 1 << 2;
}  // namespace mamba

#endif  // MAMBA_FETCH_HPP
