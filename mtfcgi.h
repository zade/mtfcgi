/*!  \file mtfcgi.h
\brief multithread fastcgi interface
\author zhaohongchao(zadezhao@qq.com)
\date 2015/3/23 19:46:45
\version 1.0.0.0
\since 1.0.0.0

simple example:

#include <mtfcgi.h>
#include <vector>
#include <boost/thread.hpp>

const int THREAD_COUNT = 4;
const int TIMEOUT_MS = 200;

struct my_consumer_data : public mf_handler{
    mtfcgi mf;
    int count;

    virtual int on_response(mf_context *ctx, mf_reader *reader, mf_writer *writer){
        return writer->write_finshed_record(ctx,NULL,0,
            "Content-type: text/html\r\n"
            "\r\n"
            "<title>FastCGI Hello!</title>"
            "<h1>FastCGI Hello!</h1>"
            "Request number %d running on host <i>%s</i>\n",
            ++count, reader->request_params()["SERVER_NAME"].c_str() );
    }
};


static int thread_consumer_run(epoll_class_t* poll,my_consumer_data* consumer){
    return consumer->mf.handler(poll->get_fd(),TIMEOUT_MS,consumer);
}

int main(){
    epoll_class_t poll;
    boost::thread_group threads;
    std::vector<my_consumer_data> consumers(THREAD_COUNT);

    for(int i = 0 ; i != THREAD_COUNT; ++ i){
        threads.add_thread(new boost::thread(thread_consumer_run, &poll, &consumers[i]));
    }

    while(poll.go()){
        poll.produce_fd();
    }

    threads.join_all();

    return 0;
}

*/
#ifndef __MTFCGI_H__
#define __MTFCGI_H__

#include "fastcgi.h"//for fastcgi protocol

#include <map> // for kvmap_t
#include <string> // for std::string
#include <stdarg.h> // for va_list
#include <sys/time.h> //for timeval
#include <vector> // for vector

/*! mtfcgi return code
*/
enum mf_status {
    MF_OK = 0,/*!< status ok . */
    MF_ERROR = -1,/*!< status error,maybe get futher error detail by errno . */
    MF_UNSUPPORTED_VERSION = -2,/*!< unsupported fastcgi version . */
    MF_PROTOCOL_ERROR = -3,/*!< fastcgi protocol error . */
    MF_PARAMS_ERROR = -4,/*!<  fastcgi parse params error. */
    MF_UNSUPPORTED_MPX_CONN = -6,/*!< don't support multiplex connnection . */
    MF_TIMEOUT_ERROR = -7,/*!< timeout error . */
    MF_HEADER_TYPE_ERROR = -8,/*!< record header type error . */
    MF_REQUSET_ID_ERROR = -9,/*!<  record header id is bad. */
    MF_READ_ERROR = -10,/*!< read fd data error, maybe get futher error detail by errno. */
    MF_WRITE_ERROR = -11,/*!< write fd data error, maybe get futher error detail by errno. */
    MF_REQUEST_ID_MISMATCH = -12,/*!<  record header id mismatch error. */
    MF_UNSUPPORTED_AUTH = -13,/*!< not support auth role(default,you can change it). */
    MF_UNSUPPORTED_FILTER = -14,/*!<  not support filter role(default,you can change it). */
};

/*! mtfcgi context
*/
struct mf_context {
    //! file descriptor
    int fd;

    //! request id for current record
    int request_id;

    //! write type, general always FCGI_STDOUT
    int write_type;

    //! app status for current handler
    int app_status;

    //! protocol status for current handler
    int protocol_status;

    //! role info
    short role;

    //! flags
    short flags;

    //! timeout time point
    timeval timeout_pt;

    //! FCGI Header
    FCGI_Header header;

    //! reset content
    void reset(int fd, int timeout_ms);

    //! ge timeout ms
    int timeout_ms() const;

    //! keep connection flag
    int keep_connection() const {
        return flags & FCGI_KEEP_CONN;
    }
};

//! string map type
typedef std::map<std::string, std::string> kvmap_t;

//! buffer type
typedef std::vector<char> mfbuf_t;

/*! mtfcgi reader
*/
class mf_reader {
    //! read params buf
    mfbuf_t params_buf_;

    //! read STDIN
    mfbuf_t request_stdin_;

    //! read DATA
    mfbuf_t request_data_;

    //! result for parse params_buf_
    kvmap_t request_params_;

  public:

    /*! read record body
    \param ctx   mf_context object with header info
    \return >0 for total bytes readed; others for error status in mf_status
    */
    int read_record_body(mf_context *ctx);

    /*! read record body for params
     \param ctx   mf_context object with header info
     \return >0 for total bytes readed; others for error status in mf_status
    */
    int read_record_params(mf_context *ctx);

    /*! read params from fd
    \param ctx   mf_context object
    \return >0 for total bytes readed; others for error status in mf_status
    */
    int read_params(mf_context *ctx);

    /*! read stdin from fd
    \param ctx   mf_context object
    \return for total bytes readed; others for error status in mf_status
    */
    int read_stdin(mf_context *ctx);

    /*! read data from fd
    \param ctx   mf_context object
    \return for total bytes readed; others for error status in mf_status
    */
    int read_data(mf_context *ctx);

    //! params result
    const kvmap_t &request_params() const {
        return request_params_;
    }

    //! params result
    kvmap_t &request_params() {
        return request_params_;
    }

    //! request_stdin result
    const mfbuf_t &request_stdin() const {
        return request_stdin_;
    }

    //! request_stdin result
    mfbuf_t &request_stdin() {
        return request_stdin_;
    }

    //! request_data result
    const mfbuf_t &request_data() const {
        return request_data_;
    }

    //! request_data result
    mfbuf_t &request_data() {
        return request_data_;
    }

    //! request_data result
    mfbuf_t &param_buf() {
        return params_buf_;
    }
};

/*! mtfcgi writer
*/
class mf_writer {
    //! writer buffer
    mfbuf_t buf_;

  public:

    /*!  write tag
    */
    enum write_tag {
        NIL,/*!<  write nothing . */
        CLOSED,/*!< write closed record . */
        FINISHED/*!< write finished record . */
    };

    //! ctor
    mf_writer();

    /*! write fastcgi record
    \param ctx   mf_context object
    \param tag   write tag
    \param data   data
    \param len   data len
    \param format   HTTP header content format
    \return return >0 for total bytes readed; others for error status in mf_status
    */
    int write_record(mf_context *ctx, write_tag tag, const void *data, int len, const char *format, ...);

    /*! write fastcgi record
    \param ctx   mf_context object
    \param tag   write tag
    \param data   data
    \param len   data len
    \param format   HTTP header content format
    \param arg   argument for format
    \return return >0 for total bytes readed; others for error status in mf_status
    */
    int write_record(mf_context *ctx, write_tag tag, const void *data, int len, const char *format, va_list arg);

    /*! write fastcgi record
    \param ctx   mf_context object
    \param data   data
    \param len   data len
    \param format   HTTP header content format
    \return return >0 for total bytes readed; others for error status in mf_status
    */
    int write_finished_record(mf_context *ctx, const void *data, int len, const char *format, ...);

    /*! write fastcgi record
    \param ctx   mf_context object
    \param data   data
    \param len   data len
    \return return >0 for total bytes readed; others for error status in mf_status
    */
    int write_finished_record(mf_context *ctx, const void *data = NULL, int len = 0) {
        return write_finished_record(ctx, data, len, NULL);
    }
};

/*! mtfcgi handler
*/
struct mf_handler {

    //! dtor
    virtual ~mf_handler() {
    }

    /*! when role is Responser
    \param ctx   mf_context object
    \param reader  mtfcgi reader
    \param writer  mtfcgi writer
    \return >=0 for ok; others for error status in mf_status
    */
    virtual int on_response(mf_context *ctx, mf_reader *reader, mf_writer *writer) = 0;

    /*! when role is Authorizer
    \param ctx   mf_context object
    \param reader  mtfcgi reader
    \param writer  mtfcgi writer
    \return >=0 for ok; others for error status in mf_status
    */
    virtual int on_auth(mf_context *ctx, mf_reader *reader, mf_writer *writer);

    /*! when role is Filter
    \param ctx   mf_context object
    \param reader  mtfcgi reader
    \param writer  mtfcgi writer
    \return >=0 for ok; others for error status in mf_status
    */
    virtual int on_filter(mf_context *ctx, mf_reader *reader, mf_writer *writer);

    /*! when called for management
    \param ctx   mf_context object
    \param reader  mtfcgi reader
    \param writer  mtfcgi writer
    \return >=0 for ok; others for error status in mf_status
    */
    virtual int on_management(mf_context *ctx, mf_reader *reader, mf_writer *writer);

    /*! when called for multiplex connect
    \param ctx   mf_context object
    \param reader  mtfcgi reader
    \param writer  mtfcgi writer
    \return >=0 for ok; others for error status in mf_status
    */
    virtual int on_multiconnect(mf_context *ctx, mf_reader *reader, mf_writer *writer);
};

/*! multithread fastcgi class
*/
struct mtfcgi {
    //! context object
    mf_context ctx;

    //! reader object
    mf_reader reader;

    //! writer object
    mf_writer writer;

    /*! handle web connection for fastcgi protocol
    \param fd   file descriptor
    \param timeout_ms   timeout in millisecond
    \param handler   customized handler
    \return  >=0 for ok; others for error status in mf_status
    */
    int handle(int fd, int timeout_ms, mf_handler *handler);
};

#endif //__MTFCGI_H__
