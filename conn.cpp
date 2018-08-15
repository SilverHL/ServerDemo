#include <exception>
#include <string.h>
#include <errno.h>

#include "conn.h"
#include "log.h"
#include "fdwrapper.h"

conn::conn()
{
    m_srvfd = -1;
    m_clt_buf = new char[BUF_SIZE];
    if ( !m_clt_buf )
    {
        throw std::exception();
    }

    m_srv_buf = new char[BUF_SIZE];
    if ( !m_srv_buf )
    {
        throw std::exception();
    }

    reset();
}

conn::~conn()
{
    delete [] m_clt_buf;
    delete [] m_srv_buf;
}

void 
conn::init_ctl( int sockfd, const sockaddr_in& client_addr )
{
    m_cltfd = sockfd;
    m_clt_address = client_addr;
}

void
conn::init_srv( int sockfd, const sockaddr_in& server_addr )
{
    m_srvfd = sockfd;
    m_srv_address = server_addr;
}

void 
conn::reset()
{
    m_clt_read_idx = 0;
    m_clt_write_idx = 0;
    m_srv_read_idx = 0;
    m_srv_write_idx = 0;
    m_srv_closed = false;
    m_cltfd = -1;
}

//此方法应该是由服务器调用 
//从客户端读取数据 然后写入到自己的缓存
//然后 服务器会将客户端缓存区的idx更新
//并且写入到自己的缓存区
RET_CODE 
conn::read_from_clt()
{
    int bytes_read = 0;
    while ( true )
    {
        if ( m_clt_read_idx >= BUF_SIZE )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "the client read buffer is full, let server write" );
            return BUFFER_FULL;
        }

        bytes_read = recv(m_cltfd, m_clt_buf + m_clt_read_idx, BUF_SIZE - m_clt_read_idx, 0);
        if (bytes_read == -1)
        {
            if ( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return IOERR;
        }
        else if (bytes_read == 0)
        {
            return CLOSED;
        }

        m_clt_read_idx += bytes_read;
    }

    return ( m_clt_read_idx - m_clt_write_idx ) > 0  ?  RET_CODE(OK) : RET_CODE(NOTHING); 
}

RET_CODE
conn::read_from_srv()
{
    int bytes_read = 0;
    while ( true )
    {
        if ( m_srv_read_idx >= BUF_SIZE )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "the server read buffer is full, let client write" );
            return BUFFER_FULL;
        }

        bytes_read = recv( m_srvfd, (m_srv_buf + m_srv_read_idx),  BUF_SIZE - m_srv_read_idx, 0);
        if ( bytes_read == -1 ) 
        {
            if ( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return IOERR;
        }
        else if ( bytes_read == 0 ) 
        {
            log ( LOG_ERR, __FILE__, __LINE__, "%s", "the server should not close the perisit connection" );
            return CLOSED;
        }

        m_srv_write_idx += bytes_read;
    }

    return ( m_srv_read_idx - m_srv_write_idx ) > 0 ? OK : NOTHING;
}

RET_CODE
conn::write_to_clt()
{
    int bytes_write = 0;
    while ( true )
    {
        if ( m_srv_read_idx <= m_srv_write_idx)
        {
            m_srv_read_idx = 0;
            m_srv_write_idx = 0;
            return BUFFER_EMPTY;
        }

        bytes_write = send( m_cltfd, m_srv_buf + m_srv_write_idx, m_srv_read_idx - m_srv_write_idx, 0);

        if (bytes_write == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return TRY_AGAIN;
            }
            log ( LOG_ERR, __FILE__, __LINE__, "write to the client socket failed %s", strerror(errno) );
            return IOERR;            
        }
        else if ( bytes_write == 0 )
        {
            return BUFFER_FULL;
        }

        m_srv_write_idx += bytes_write;
    }
}

RET_CODE
conn::write_to_srv()
{
    int bytes_write = 0;
    while ( true )
    {
        if ( m_clt_read_idx <= m_clt_write_idx)
        {
            m_clt_read_idx = 0;
            m_clt_write_idx = 0;
            return BUFFER_EMPTY;
        }

        bytes_write = send( m_srvfd, ( m_clt_buf + m_clt_write_idx ), ( m_clt_write_idx - m_clt_read_idx ), 0);

        if ( bytes_write == -1 )
        {
            if ( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                return TRY_AGAIN;
            }
            log( LOG_ERR, __FILE__, __LINE__, "write to the server failed %s", strerror(errno ) );
        }
        else if (bytes_write == 0)
        {
            return BUFFER_FULL;
        }

        m_clt_write_idx += bytes_write;
    }
}
