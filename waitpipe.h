#if !defined(INCLUDED_WAITPIPE_H_)
#define INCLUDED_WAITPIPE_H_

class WaitPipe
{
    int m_in, m_out;

public:
    WaitPipe();
    ~WaitPipe();

    int WaitFd();
    void Notify();
    void Exit();
};

#endif
