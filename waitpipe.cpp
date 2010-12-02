#include "waitpipe.h"
#include <fcntl.h>
#include <unistd.h>

WaitPipe::WaitPipe()
{
    int fds[2];
    pipe(fds);
    m_out = fds[0];
    m_in = fds[1];
    fcntl(m_in, F_SETFL, O_NONBLOCK);
}


WaitPipe::~WaitPipe()
{
    close(m_in);
    close(m_out);
}


int WaitPipe::WaitFd()
{
    return m_out;
}


void WaitPipe::Notify()
{
    write(m_in,"n",1);
}

void WaitPipe::Exit()
{
    write(m_in,"y",1);
}
