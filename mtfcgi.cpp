/*!  \file mtfcgi.cpp
\brief multithread fastcgi implementation
\author zhaohongchao(zadezhao@qq.com)
\date 2015/3/23 20:06:28
\version 1.0.0.0
\since 1.0.0.0
*/
#include "mtfcgi.h"
#include <poll.h>//for poll function
#include <assert.h>// for assert
#include <string.h>//for memset
#include <stdio.h>//for vsnprintf
#include <errno.h>//for errno

//#include <ComLogger.h>

//! anonymouse namespace
namespace {

enum {
    WRITER_BUF_SIZE = 0xFFF8,/*!< default writer buffer size . */
    READER_BUF_SIZE = 8192,/*!< default read buffer size . */
};

//! to int len
template<class T>
int to_int_(T len) {
    return static_cast<int>(len);
}

//! check fd is ready for read or write
int is_fd_ready_(mf_context *ctx, int events) {
    int ret = MF_OK;
    struct pollfd pfd;
    pfd.fd = ctx->fd;
    pfd.events = events | POLLERR | POLLHUP;

    while (true) {
        int timeout = ctx->timeout_ms();

        if (timeout < 0) {
            ret = MF_TIMEOUT_ERROR;
            break;
        }

        ret = poll(&pfd, 1, timeout);

        if (ret > 0) {
            ret = MF_OK;
            break;
        } else if (ret == 0) {
            ret = MF_TIMEOUT_ERROR;
            break;
        } else  if (EINTR != errno) {
            ret = MF_ERROR;
            break;
        }
    }

    return ret;
}

//! read data by timeout
int read_data_(mf_context *ctx, void *vbuf, int len) {
    char *buf = reinterpret_cast<char *>(vbuf);
    int readed = 0;

    while (len > 0) {
        int ret = is_fd_ready_(ctx, POLLIN);

        if (ret == MF_OK) {
            ret = ::read(ctx->fd, buf, len);

            if (ret > 0) {
                readed += ret;
                buf += ret;
                len -= ret;
            } else {
                readed = MF_READ_ERROR;
                break;
            }
        } else {
            readed = ret;
            break;
        }
    }

    //WRITE_LOG(LOG_DEBUG, "read data %d %d", len, readed);

    return readed;
}

//! write data by timeout
int write_data_(mf_context *ctx, const void *vbuf, int len) {
    const char *buf = reinterpret_cast<const char *>(vbuf);
    int writed = 0;

    while (len > 0) {
        int ret = is_fd_ready_(ctx, POLLOUT);

        if (ret == MF_OK) {
            ret = ::write(ctx->fd, buf, len);

            if (ret > 0) {
                writed += ret;
                buf += ret;
                len -= ret;
            } else {
                writed = MF_WRITE_ERROR;
                break;
            }
        } else {
            writed = ret;
            break;
        }
    }

    //WRITE_LOG(LOG_DEBUG, "write data %d %d", len, writed);

    return writed;
}

//! make fastcgi header
void set_header_(void *hd, int type, int id, int content_len, int padding_len) {
    assert(content_len >= 0 && content_len <= FCGI_MAX_LENGTH);
    assert(padding_len >= 0 && padding_len <= 0xff);
    FCGI_Header *header = reinterpret_cast<FCGI_Header *>(hd);
    header->version = FCGI_VERSION_1;
    header->type = static_cast<unsigned char>(type);
    header->requestIdB1 = static_cast<unsigned char>((id >> 8) & 0xff);
    header->requestIdB0 = static_cast<unsigned char> ((id) & 0xff);
    header->contentLengthB1 = static_cast<unsigned char> ((content_len >> 8) & 0xff);
    header->contentLengthB0 = static_cast<unsigned char> ((content_len) & 0xff);
    header->paddingLength = static_cast<unsigned char>(padding_len);
    header->reserved = 0;
}

void set_end_request_(void *rd, int id, int app_status, int protocol_status) {
    FCGI_EndRequestRecord *record = reinterpret_cast<FCGI_EndRequestRecord *>(rd);
    set_header_(&record->header, FCGI_END_REQUEST, id, to_int_(sizeof(record->body)), 0);
    record->body.appStatusB3 = static_cast<unsigned char>((app_status >> 24) & 0xff);
    record->body.appStatusB2 = static_cast<unsigned char>((app_status >> 16) & 0xff);
    record->body.appStatusB1 = static_cast<unsigned char>((app_status >> 8) & 0xff);
    record->body.appStatusB0 = static_cast<unsigned char>((app_status) & 0xff);
    record->body.protocolStatus = static_cast<unsigned char>(protocol_status);
    memset(record->body.reserved, 0, sizeof(record->body.reserved));
}

//! get reqeust id by header
int get_request_id_(const FCGI_Header &header) {
    return (header.requestIdB1 << 8) + header.requestIdB0;
}

//! get request length by header
int get_length_(const FCGI_Header &header) {
    return (header.contentLengthB1 << 8) + header.contentLengthB0;
}

//! read record body with header info
int read_record_body_(mf_context *ctx, const FCGI_Header &header, mfbuf_t &buf) {
    int ret = 0;

    if (get_request_id_(header) == ctx->request_id) {
        const int content_len = get_length_(header);
        const int len = content_len + header.paddingLength;

        if (len > 0) {
            if (buf.capacity() == 0) {
                buf.reserve(READER_BUF_SIZE);
            }

            const size_t prev_size = buf.size();
            buf.resize(prev_size + len);
            ret = read_data_(ctx, &buf[prev_size], len);

            if (ret == len && header.paddingLength > 0) {
                buf.resize(prev_size + content_len);
            }
        }
    } else {
        ret = MF_REQUEST_ID_MISMATCH;
    }

    return ret;
}

//! read request by timeout
int read_record_(mf_context *ctx, int type, mfbuf_t &data) {
    int total_len = 0;
    int ret = 0;

    while (true) {
        FCGI_Header  header;
        ret = read_data_(ctx, &header, FCGI_HEADER_LEN);

        if (ret != FCGI_HEADER_LEN) {
            break;
        } else if (header.version != FCGI_VERSION_1) {
            ret = MF_UNSUPPORTED_VERSION;
            break;
        } else if (FCGI_BEGIN_REQUEST == header.type) {
            ret = MF_UNSUPPORTED_MPX_CONN;
            break;
        } else if (type != header.type) {
            ret = MF_HEADER_TYPE_ERROR;
            break;
        }

        total_len += FCGI_HEADER_LEN;

        ret = read_record_body_(ctx, header, data);

        if (ret > 0) {
            total_len += ret;
        } else {
            break;
        }
    }

    return (ret == 0 ? total_len : ret);
}


//! parse param length
int parse_params_len_(const char *&pos, const char *end) {
    const unsigned char *upos = reinterpret_cast<const unsigned char *>(pos);
    const unsigned char *uend = reinterpret_cast<const unsigned char *>(end);
    const unsigned char *ptr = upos;
    int len = *ptr++;

    if ((len & 0x80) != 0) {
        if (uend - ptr < 3) {
            return -1;
        }

        len = ((len & 0x7f) << 24) + (ptr[0] << 16) + (ptr[1] << 8) + ptr[2];
        ptr += 3;
    }

    if (ptr > uend) {
        return -1;
    }

    pos += ptr - upos;

    return len;
}

//! parse params from buffer
int parse_params_(const mfbuf_t &buf, kvmap_t &kvs) {
    const char *pos = &buf.front();
    const char *end = pos + buf.size();

    while (pos < end) {
        int name_len = parse_params_len_(pos, end);

        if (name_len < 0) {
            return MF_PARAMS_ERROR;
        }

        int value_len = parse_params_len_(pos, end);

        if (value_len < 0) {
            return MF_PARAMS_ERROR;
        }

        const char *name_end = pos + name_len;
        const char *value_end = name_end + value_len;

        if (value_end <= end) {
            const std::string key(pos, name_end);
            const std::string value(name_end, value_end);
            if (!key.empty() && !value.empty()){
                kvs.insert(std::make_pair(key, value));
            }            
            pos = value_end;
        } else {
            return MF_PARAMS_ERROR;
        }
    }

    return MF_OK;
}

//! make aligned int 8 bytes
int align_int8_(int n) {
    return (n + 7) & 0xFFFFFFF8;
}
}

//////////////////////////////////////////////////////////////////////////
void mf_context::reset(int fd, int timeout_ms) {
    this->fd = fd;
    request_id = 0;
    write_type = FCGI_STDOUT;
    app_status = MF_OK;
    protocol_status = FCGI_REQUEST_COMPLETE;

    gettimeofday(&timeout_pt, NULL);
    timeout_pt.tv_sec += timeout_ms / 1000;
    timeout_pt.tv_usec += (timeout_ms % 1000) * 1000;
}

int mf_context::timeout_ms() const {
    timeval now;
    gettimeofday(&now, NULL);
    return (timeout_pt.tv_sec - now.tv_sec) * 1000 + (timeout_pt.tv_usec - now.tv_usec) / 1000;
}

//////////////////////////////////////////////////////////////////////////
int mf_reader::read_record_body(mf_context *ctx) {
    params_buf_.clear();
    return read_record_body_(ctx, ctx->header, params_buf_);
}

int mf_reader::read_record_params(mf_context *ctx) {
    request_params_.clear();
    params_buf_.clear();

    int len = read_record_body_(ctx, ctx->header, params_buf_);

    if (len > 0) {
        int ret = parse_params_(params_buf_, request_params_);

        if (ret < 0) {
            return ret;
        }
    }

    return len;
}

int mf_reader::read_params(mf_context *ctx) {
    request_params_.clear();
    params_buf_.clear();

    int len = read_record_(ctx, FCGI_PARAMS, params_buf_);

    if (len > 0) {
        int ret = parse_params_(params_buf_, request_params_);

        if (ret < 0) {
            return ret;
        }
    }

    return len;
}

int mf_reader::read_stdin(mf_context *ctx) {
    request_stdin_.clear();
    return read_record_(ctx, FCGI_STDIN, request_stdin_);
}

int mf_reader::read_data(mf_context *ctx) {
    request_data_.clear();
    return read_record_(ctx, FCGI_DATA, request_data_);
}
//////////////////////////////////////////////////////////////////////////
mf_writer::mf_writer() {
    buf_.resize(WRITER_BUF_SIZE);
}

int mf_writer::write_record(mf_context *ctx, write_tag tag, const void *data, int len, const char *format, ...) {
    va_list vl;
    va_start(vl, format);
    int ret = write_record(ctx, tag, data, len, format, vl);
    va_end(vl);
    return ret;
}

int mf_writer::write_finished_record(mf_context *ctx, const void *data, int len, const char *format, ...) {
    va_list vl;
    va_start(vl, format);
    int ret = write_record(ctx, FINISHED, data, len, format, vl);
    va_end(vl);
    return ret;
}

int mf_writer::write_record(mf_context *ctx, write_tag tag, const void *data, int len, const char *format, va_list arg) {
    if (len < 0) {
        return MF_WRITE_ERROR;
    }

    const int buf_len = to_int_(buf_.size());
    int used_len = FCGI_HEADER_LEN;
    int left_len = buf_len - used_len;
    int total_len = 0;

    if (format) {
        const int writed = vsnprintf(&buf_[used_len], static_cast<size_t>(left_len), format, arg);

        if (writed <= 0 || writed >= left_len) {
            return MF_WRITE_ERROR;
        }

        used_len += writed;
    }

    const char *cdata = reinterpret_cast<const char *>(data);
    bool write_tail = (tag != NIL);

    do {
        left_len = buf_len - used_len;
        const int body_len = (left_len > len ? len : left_len);

        if (body_len) {
            memcpy(&buf_[used_len], cdata, body_len);
            len -= body_len;
            cdata += body_len;
            used_len += body_len;
        }

        const int content_len = used_len - FCGI_HEADER_LEN;
        const int padding_len = align_int8_(content_len) - content_len;
        set_header_(&buf_[0], ctx->write_type, ctx->request_id, content_len, padding_len);
        int raw_len = used_len + padding_len;

        if (buf_len > raw_len && write_tail) {
            const int left_raw_len = buf_len - raw_len;
            const int record_len = to_int_(sizeof(FCGI_EndRequestRecord));
            const bool has_content = (content_len != 0);
            const int tail_len = (tag == FINISHED ? record_len : 0) + (has_content ? FCGI_HEADER_LEN : 0);

            if (tail_len <= left_raw_len) {
                if (has_content) { //avoid writing one more empty headers
                    set_header_(&buf_[raw_len], ctx->write_type, ctx->request_id, 0, 0);
                    raw_len += FCGI_HEADER_LEN;
                }

                if (tag == FINISHED) {
                    set_end_request_(&buf_[raw_len], ctx->request_id, ctx->app_status, ctx->protocol_status);
                    raw_len += record_len;
                }

                write_tail = false;
            }
        }

        int ret = write_data_(ctx, &buf_[0], raw_len);

        if (ret != raw_len) {
            return ret;
        }

        total_len += raw_len;
        used_len = FCGI_HEADER_LEN;
    } while (len > 0 || write_tail);

    return total_len;
}
//////////////////////////////////////////////////////////////////////////
int mf_handler::on_auth(mf_context *ctx, mf_reader *reader, mf_writer *writer) {
    ctx->app_status = MF_UNSUPPORTED_AUTH;
    return writer->write_finished_record(ctx);
}

int mf_handler::on_filter(mf_context *ctx, mf_reader *reader, mf_writer *writer) {
    ctx->app_status = MF_UNSUPPORTED_FILTER;
    return writer->write_finished_record(ctx);
}

int mf_handler::on_multiconnect(mf_context *ctx, mf_reader *reader, mf_writer *writer) {
    ctx->protocol_status = FCGI_CANT_MPX_CONN;
    return writer->write_finished_record(ctx);
}

int mf_handler::on_management(mf_context *ctx, mf_reader *reader, mf_writer *writer) {
    assert(ctx->header.type == FCGI_GET_VALUES);
    int ret = reader->read_record_params(ctx);

    if (ret > 0) {
        char buf[64]; /* 64 = 8 + 3*(1+1+14+1)* + padding */
        char *buf_end = &buf[0];
        const kvmap_t &params = reader->request_params();

        for (kvmap_t::const_iterator itr = params.begin(), end = params.end(); itr != end; ++itr) {
            char value = '\0';

            if (itr->first == FCGI_MAX_CONNS) {
                value = '1';
            } else if (itr->first == FCGI_MAX_REQS) {
                value = '1';
            } else if (itr->first == FCGI_MPXS_CONNS) {
                value = '0';
            }

            if (value != '\0') {
                int len = itr->first.length();
                sprintf(buf_end, "%c%c%s%c", len, 1, itr->first.c_str(), value);
                buf_end += len + 3;
            }
        }

        ctx->write_type = FCGI_GET_VALUES_RESULT;
        ret = writer->write_finished_record(ctx, &buf[0], to_int_(buf_end - buf));
    }

    return ret;
}

//////////////////////////////////////////////////////////////////////////
int mtfcgi::handle(int fd, int timeout_ms, mf_handler *handler) {
    ctx.reset(fd, timeout_ms);

    while (true) {
        if ((ctx.app_status = read_data_(&ctx, &ctx.header, FCGI_HEADER_LEN)) != FCGI_HEADER_LEN) {
            break;
        }

        ctx.request_id = get_request_id_(ctx.header);
        //WRITE_LOG(LOG_DEBUG, "type %d, id %d", ctx.header.type, ctx.request_id);

        if (ctx.header.type == FCGI_BEGIN_REQUEST) {//handle app reqeust
            //check id!=0
            if (ctx.request_id == FCGI_NULL_REQUEST_ID) {
                ctx.app_status = MF_REQUSET_ID_ERROR;
                break;
            }

            //read body info
            FCGI_BeginRequestBody body;
            const int body_len = to_int_(sizeof(body));

            if ((ctx.app_status = read_data_(&ctx, &body, body_len)) != body_len) {
                break;
            }

            ctx.role = (body.roleB1 << 8) + body.roleB0;
            ctx.flags = body.flags;

            // read params info
            if ((ctx.app_status = reader.read_params(&ctx)) < 0) {
                break;
            }

            switch (ctx.role) {//handle role request
                case FCGI_RESPONDER:
                    if ((ctx.app_status = reader.read_stdin(&ctx)) > 0) {
                        ctx.app_status = handler->on_response(&ctx, &reader, &writer);
                    }

                    break;

                case FCGI_AUTHORIZER:
                    ctx.app_status = handler->on_auth(&ctx, &reader, &writer);
                    break;

                case FCGI_FILTER:
                    if ((ctx.app_status = reader.read_stdin(&ctx)) > 0 && (ctx.app_status = reader.read_data(&ctx)) > 0) {
                        ctx.app_status = handler->on_filter(&ctx, &reader, &writer);
                    }

                    break;

                default://bad role
                    ctx.protocol_status = FCGI_UNKNOWN_ROLE;
                    ctx.app_status = writer.write_finished_record(&ctx);
                    break;
            }

            break;
        } else if (ctx.request_id == FCGI_NULL_REQUEST_ID) {//handle management request
            if (ctx.header.type == FCGI_GET_VALUES) {
                ctx.app_status = handler->on_management(&ctx, &reader, &writer);
            } else {
                ctx.write_type = FCGI_UNKNOWN_TYPE;
                FCGI_UnknownTypeBody body;
                body.type = ctx.header.type;
                memset(body.reserved, 0, sizeof(body.reserved));
                ctx.app_status = writer.write_finished_record(&ctx, &body, to_int_(sizeof(body)));
            }

            break;
        } else {//handle ignored request
            //WRITE_LOG(LOG_DEBUG, "ignored type %d,length %d", ctx.header.type, ctx.app_status);
            if ((ctx.app_status = reader.read_record_body(&ctx)) < 0) {
                break;
            }
        }
    }

    if (ctx.app_status == MF_UNSUPPORTED_MPX_CONN) {
        ctx.app_status = handler->on_multiconnect(&ctx, &reader, &writer);
    }

    return ctx.app_status;
}

