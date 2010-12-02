/* (c) Peter Chapman 2010
   Licensed under the GNU General Public Licence; either version 2 or (at your
   option) any later version
   
   Thanks to Tejun Heo <tj@kernel.org> for the cusexmp sample program and ossp,
   both of which were very useful in writing this.
*/

#define FUSE_USE_VERSION 29

#include <cuse_lowlevel.h>
#include <fuse_opt.h>
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <linux/joystick.h>
#include <libxml/parser.h>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/format.hpp>
#include <boost/bind.hpp>
#include <map>
#include <deque>
#include <string>
#include <iostream>
#include <vector>
#include <stdexcept>
#include "waitpipe.h"
#include "joymodel.h"

struct stickshift_param {
        int             major;
        int             minor;
        const char     *indev;
        const char     *outdev;
        const char     *configfile;
        const char     *calibratedfile;
        int             is_help;
} g_params = stickshift_param();

static const char *usage =
"usage: stickshift [options]\n"
"\n"
"options:\n"
"    --help | -h             print this help message\n"
"    --maj=MAJ | -M MAJ      output joystick device major number\n"
"    --min=MIN | -m MIN      output joystick device minor number\n"
"    --indev=DEV | -I DEV    real joystick device\n"
"    --outdev=DEV | -O DEV   use major/minor device numbers from DEV (must \n"
"                            exist first)\n"
"    --config=CFG            XML configuration file\n"
"    --calibrated=CFG        output XML config file (written if virtual\n"
"                            joystick is calibrated)\n"
"\n";


pthread_t selectThread;
WaitPipe wakePipe;

// RAII lock for pthread
struct Lock {
    pthread_mutex_t &mutex;
    bool locked;
    Lock(pthread_mutex_t &mutex) : mutex(mutex), locked(false) { lock(); }
    ~Lock() { unlock(); }
    void lock()   { if (!locked) pthread_mutex_lock(&mutex); locked = true; }
    void unlock() { if (locked) pthread_mutex_unlock(&mutex); locked = false; }
};

// An object of this type represents an open descriptor on our cuse device. So
// if two programs open the joystick simultaneously, we get two independent
// JsFile objects.
class JsFile
{
    int                  m_fd;       // descriptor of "real" joystick
    __u32                m_version;
    std::deque<js_event> m_events;   // output event queue
    
    // outstanding read request, for blocking reads
    fuse_req_t            m_readReq;
    size_t                m_readSize; // Size requested
    
    // used to inform fuse when input is available, if client is doing
    // select/poll on our device
    fuse_pollhandle      *m_pollHandle;

    // Sync between fuse thread and selectThread
    pthread_mutex_t       m_mutex;
    
    // This is a model of the real joystick - input events on the real device
    // are emitted as signals on the buttons & axes of this object.
    boost::shared_ptr<InputJoystick> m_inputJoystick;
    
    // This is the 'virtual' joystick. It attaches itself to m_inputJoystick
    // and presents a modified configuration of axes & buttons
    boost::shared_ptr<Joystick>      m_outputJoystick;
    
    // Called by m_outputJoystick to output an event to the virtual joystick
    void AddEvent(__u32 time, __s16 value, __u8 type, bool init, __u8 number);
    
    // Process an individual input event on the real joystick
    void Input(const js_event &e);
    
    // Read & process all input events from real joystick
    void ReadAllInput();
    
    // Attempt to fulful outstanding read request on virtual joystick device
    bool AttemptOutput();
    
    // Called by fuse when existing read request is interrupted
    void ReadInterrupted(fuse_req_t req);
    
    static void read_interrupted(fuse_req_t req, void *data);
    
public:
    Joystick &GetJoystick() { return *m_outputJoystick; }
    __u32     Version()     { return m_version; }
    int       InputFd()     { return m_fd; }
    
    void Read(fuse_req_t req, size_t size, fuse_file_info *fi);
    void Poll(fuse_req_t req, struct fuse_pollhandle *ph);
    
    // Called when data is available on input FD
    void ReadAvailable();
    
    // Are we waiting for input to become available on input FD?
    bool WantInput() const;

    JsFile(const char *inputDev, const char *configFile, const char *configOut);
    ~JsFile();
    
};
typedef boost::shared_ptr<JsFile> JsFilePtr;

JsFile::JsFile(const char *inputDev, const char *configFile,
               const char *configOut)
    : m_fd(-1),
      m_version(0),
      m_readReq(0),
      m_pollHandle(0)
{
    m_fd = open(inputDev, O_RDONLY | O_NONBLOCK);
    if (m_fd < 0)
        throw std::runtime_error("Can't open input device");
    
    ioctl(m_fd, JSIOCGVERSION, &m_version);
    
    m_inputJoystick.reset(new InputJoystick(m_fd));
    m_outputJoystick.reset(new MappedJoystick(m_inputJoystick,
                                              configFile,
                                              configOut));
    
    pthread_mutex_init(&m_mutex, NULL);
    
    
    // Have all axes & buttons on virtual joystick call AddEvent
    using boost::bind;
    for (unsigned i = 0; i < m_outputJoystick->NumButtons(); ++i)
    {
        m_outputJoystick->GetButton(i)->Connect(
                bind(&JsFile::AddEvent, this,
                      _1, _2, JS_EVENT_BUTTON, _3, i));
    }
    for (unsigned i = 0; i < m_outputJoystick->NumAxes(); ++i)
    {
        m_outputJoystick->GetAxis(i)->Connect(
                bind(&JsFile::AddEvent, this,
                     _1, _2, JS_EVENT_AXIS, _3, i));
    }
}

void JsFile::AddEvent(__u32 time, __s16 value, __u8 type, bool init,
                       __u8 number)
{
    js_event e = { time, value, type | (init ? JS_EVENT_INIT : 0), number };
    m_events.push_back(e);
}

void JsFile::Input(const js_event &e)
{
    bool init = e.type & JS_EVENT_INIT;
    switch (e.type & ~JS_EVENT_INIT)
    {
        case JS_EVENT_BUTTON:
            if (e.number < m_inputJoystick->NumButtons())
                m_inputJoystick->GetButton(e.number)->Input(
                        e.time, e.value, init);
            break;
        case JS_EVENT_AXIS:
            if (e.number < m_inputJoystick->NumAxes())
                m_inputJoystick->GetAxis(e.number)->Input(
                        e.time, e.value, init);
            break;
    }
}

void JsFile::ReadAllInput()
{
    // m_fd is non-blocking, so just read as much as we can
    js_event event;
    while (read(m_fd, &event, sizeof(event)) == sizeof(event))
        Input(event);
}

bool JsFile::WantInput() const
{
    return m_readReq || m_pollHandle;
}

void JsFile::ReadAvailable()
{
    Lock l(m_mutex);

    ReadAllInput();
    
    if (m_pollHandle && !m_events.empty())
    {
        fuse_notify_poll(m_pollHandle);
        fuse_pollhandle_destroy(m_pollHandle);
        m_pollHandle = 0;
    }
    
    if (m_readReq)
    {
        AttemptOutput();
    }
}

struct EventOrder {
    bool operator()(const js_event &a, const js_event &b) const {
        if (a.time != b.time) return a.time < b.time;
        if (a.type != b.type) return a.type < b.type;
        if (a.number != b.number) return a.number < b.number;
        return false;
    }
};

bool JsFile::AttemptOutput()
{
    std::stable_sort(m_events.begin(), m_events.end(), EventOrder());
    assert(m_readReq);
    size_t eventsWanted = m_readSize/sizeof(js_event);
    size_t eventsToSend = std::min(eventsWanted, m_events.size());
    if (eventsToSend > 0)
    {
        std::deque<js_event>::iterator i = m_events.begin() + eventsToSend;
        // need events in a contiguous area of memory
        std::vector<js_event> buf(m_events.begin(), i);
        m_events.erase(m_events.begin(), i);
        fuse_reply_buf(m_readReq, (char*)buf.data(),
                       buf.size()*sizeof(js_event));
        m_readReq = 0;
        return true;
    }
    return false;
}

void JsFile::Read(fuse_req_t req, size_t size, fuse_file_info *fi)
{
    Lock l(m_mutex);
    m_readReq = req;
    m_readSize = size;
    
    ReadAllInput();
    if (AttemptOutput()) {
        return; // Success! Returned something at least.
    } else if (fi->flags & O_NONBLOCK) {
        // We were opened in non-blocking mode & have nothing right now
        fuse_reply_err(m_readReq, EWOULDBLOCK);
        m_readReq = 0;
        return;
    }
    
    // Set fn to be called if this read is interrupted
    fuse_req_interrupt_func(req, &JsFile::read_interrupted, this);
    
    // Make sure IO thread has seen us
    wakePipe.Notify();
}

void JsFile::Poll(fuse_req_t req, struct fuse_pollhandle *ph)
{
    Lock l(m_mutex);
    
    if (ph)
    {
        if (m_pollHandle && ph != m_pollHandle) {
            // Only keep 1 pollhandle going at any one time
            fuse_pollhandle_destroy(m_pollHandle);
        }
        m_pollHandle = ph;
    }
    
    unsigned revents = 0;
    if (!m_events.empty())
        revents |= POLLIN; // input available now
    
    fuse_reply_poll(req, revents);
    
    if (m_pollHandle)
        wakePipe.Notify(); // fuse wants to know when more input is available
}

void JsFile::ReadInterrupted(fuse_req_t req)
{
    assert(req == m_readReq);
    fuse_reply_err(req, EINTR);
}

void JsFile::read_interrupted(fuse_req_t req, void *data)
{
    ((JsFile*)data)->ReadInterrupted(req);
}

JsFile::~JsFile()
{
    if (m_fd >= 0)
        close(m_fd);
}
    
typedef std::map<uint64_t, JsFilePtr> FileHandleMap;
FileHandleMap s_fileHandles;
pthread_mutex_t s_fileHandlesMutex = PTHREAD_MUTEX_INITIALIZER;

void *select_threadproc(void *)
{
    fd_set fds;
    const int wakeFd = wakePipe.WaitFd();
    for (char exit = 'n'; exit != 'y';)
    {
        FD_ZERO(&fds);
        int maxfd = wakeFd;
        FD_SET(wakeFd, &fds);
        
        Lock l(s_fileHandlesMutex);
        for (FileHandleMap::const_iterator i = s_fileHandles.begin();
             i != s_fileHandles.end(); ++i)
        {
            if (i->second->WantInput()) {
                int fd = i->second->InputFd();
                FD_SET(fd, &fds);
                maxfd = std::max(maxfd, fd);
            }
        }
        l.unlock();
        
        int sel = select(maxfd+1, &fds, 0, 0, 0);
        
        if (FD_ISSET(wakeFd, &fds))
        {
            read(wakeFd, &exit, 1);
            continue;
        }
        
        l.lock();
        for (FileHandleMap::const_iterator i = s_fileHandles.begin();
             i != s_fileHandles.end(); ++i)
        {
            int fd = i->second->InputFd();
            if (fd <= maxfd && FD_ISSET(fd, &fds))
                i->second->ReadAvailable();
        }
    }
    return 0;
}


static void stickshift_open(fuse_req_t req, struct fuse_file_info *fi)
{
    try {
        JsFilePtr joy(new JsFile(g_params.indev, g_params.configfile,
                                 g_params.calibratedfile));
        Lock l(s_fileHandlesMutex);
        while (s_fileHandles.find(fi->fh) != s_fileHandles.end())
            ++fi->fh;
        
        s_fileHandles[fi->fh] = joy;
        fi->direct_io = 1; // lets us reply to reads with a short count
        fuse_reply_open(req, fi);
        return;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
    }
    catch (...)
    {
        std::cerr << "Unknown exception\n";
    }
    fuse_reply_err(req, ENODEV);
}

static void stickshift_release(fuse_req_t req, struct fuse_file_info *fi)
{
    Lock l(s_fileHandlesMutex);
    bool ok = s_fileHandles.erase(fi->fh) > 0;
    fuse_reply_err(req, ok ? 0 : EINVAL);
}

static void stickshift_read(fuse_req_t req, size_t size, off_t off,
                         struct fuse_file_info *fi)
{
    s_fileHandles[fi->fh]->Read(req, size, fi);
}

static void stickshift_ioctl(fuse_req_t req, int cmd, void *arg,
                          struct fuse_file_info *fi, unsigned flags,
                          const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
    JsFile &file = *s_fileHandles[fi->fh];
    Joystick &joy = file.GetJoystick();
    
    unsigned cmdsize = _IOC_SIZE(cmd);
    switch (cmd & ~IOCSIZE_MASK)
    {
    case JSIOCGNAME(0): {
        std::string name = joy.GetName();
        size_t len = name.empty() ? 0 : name.size()+1;
        if (!out_bufsz) {
            struct iovec iov = { arg, len };
            fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
        } else {
            fuse_reply_ioctl(req, 0, name.c_str(), len);
        }
        break;
    }
    case (JSIOCGVERSION & ~IOCSIZE_MASK):
        if (!out_bufsz) {
            struct iovec iov = { arg, sizeof(__u32) };
            fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
        } else {
            __u32 version = file.Version();
            fuse_reply_ioctl(req, 0, &version, sizeof(__u32));
        }
        break;
    case (JSIOCGAXES & ~IOCSIZE_MASK):
        if (!out_bufsz) {
            struct iovec iov = { arg, sizeof(__u8) };
            fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
        } else {
            __u8 axes = joy.NumAxes();
            fuse_reply_ioctl(req, 0, &axes, sizeof(__u8));
        }
        break;
    case (JSIOCGBUTTONS & ~IOCSIZE_MASK):
        if (!out_bufsz) {
            struct iovec iov = { arg, sizeof(__u8) };
            fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
        } else {
            __u8 buttons = joy.NumButtons();
            fuse_reply_ioctl(req, 0, &buttons, sizeof(__u8));
        }
        break;
    case (JSIOCGAXMAP & ~IOCSIZE_MASK):
        if (!out_bufsz) {
            struct iovec iov = { arg, cmdsize };
            fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
        } else {
            unsigned entries = cmdsize;
            __u8 map[entries];
            int toFill = std::min(entries, joy.NumAxes());
            
            for (unsigned i = 0; i < toFill; ++i)
                map[i] = joy.GetAxis(i)->GetMapping();
            fuse_reply_ioctl(req, 0, map, cmdsize);
        }
        break;
    case (JSIOCGBTNMAP & ~IOCSIZE_MASK):
        if (!out_bufsz) {
            struct iovec iov = { arg, cmdsize };
            fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
        } else {
            unsigned entries = cmdsize/sizeof(__u16);
            __u16 map[entries];
            unsigned toFill = std::min(entries, joy.NumButtons());
            
            for (unsigned i = 0; i < toFill; ++i)
                map[i] = joy.GetButton(i)->GetMapping();
            
            fuse_reply_ioctl(req, 0, map, cmdsize);
        }
        break;
    case (JSIOCGCORR & ~IOCSIZE_MASK): {
        size_t len = joy.NumAxes() * sizeof(js_corr);
        if (!out_bufsz || out_bufsz < len) {
            struct iovec iov = { arg, len };
            fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
        } else {
            js_corr *j = new js_corr[joy.NumAxes()];
            joy.GetCorrection(j);
            fuse_reply_ioctl(req, 0, j, len);
            delete [] j;
        }
       break;
    }
    case (JSIOCSCORR & ~IOCSIZE_MASK): {
        size_t len = joy.NumAxes() * sizeof(js_corr);
        if (!in_bufsz || in_bufsz < len) {
            struct iovec iov = { arg, len };
            fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        } else {
            joy.SetCorrection((const js_corr *)in_buf);
            fuse_reply_ioctl(req, 0, 0, 0);
        }
       break;
    }
    default:
        std::cerr << "unknown ioctl "<< cmd << cmd << '\n';
        fuse_reply_err(req, EINVAL);
        break;
    }
}

static void stickshift_poll(fuse_req_t req, struct fuse_file_info *fi,
                         struct fuse_pollhandle *ph)
{
    s_fileHandles[fi->fh]->Poll(req, ph);
}

void stickshift_init(void *userdata, struct fuse_conn_info *conn)
{
    LIBXML_TEST_VERSION
    if (pthread_create(&selectThread, NULL, &select_threadproc, 0) != 0)
    {
        std::cerr << "Can't create thread\n";
        exit(1);
    }
}

void stickshift_destroy(void *userdata)
{
    wakePipe.Exit();
    pthread_join(selectThread, 0);
    xmlCleanupParser();
}

#define SSHIFT_OPT(t, p) { t, offsetof(struct stickshift_param, p), 1 }

static const struct fuse_opt stickshift_opts[] = {
        SSHIFT_OPT("-M %u",             major),
        SSHIFT_OPT("--maj=%u",          major),
        SSHIFT_OPT("-m %u",             minor),
        SSHIFT_OPT("--min=%u",          minor),
        SSHIFT_OPT("-I %s",             indev),
        SSHIFT_OPT("--indev=%s",        indev),
        SSHIFT_OPT("-O %s",             outdev),
        SSHIFT_OPT("--outdev=%s",       outdev),
        SSHIFT_OPT("-c %s",             configfile),
        SSHIFT_OPT("--config=%s",       configfile),
        SSHIFT_OPT("--calibrated=%s",   calibratedfile),
        FUSE_OPT_KEY("-h",              0),
        FUSE_OPT_KEY("--help",          0),
        {0, 0, 0}
};

static int stickshift_process_arg(void *data, const char *arg, int key,
                               struct fuse_args *outargs)
{
    struct stickshift_param *param = (stickshift_param*)data;

    (void)outargs;
    (void)arg;

    switch (key)
    {
    case 0:
        param->is_help = 1;
        std::cerr << usage;
        return fuse_opt_add_arg(outargs, "-ho");
    default:
        return 1;
    }
}

int main(int argc, char **argv)
{
    using namespace std;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct cuse_lowlevel_ops stickshift_clop = cuse_lowlevel_ops();
    struct cuse_info ci = cuse_info();
    g_params.major = g_params.minor = -1;
    
    char *pcwd = get_current_dir_name();
    string cwd(pcwd);
    free(pcwd);
    
    if (fuse_opt_parse(&args, &g_params, stickshift_opts,
                       stickshift_process_arg))
    {
        cerr << "failed to parse options\n";
        return 1;
    }
    
    if (g_params.is_help)
        goto help_fast_exit;

    if (!(g_params.indev))
    {
        cerr << "no input joystick device specified\n";
        return 1;
    }
    if (!g_params.configfile)
    {
        cerr << "no config file specified\n";
        return 1;
    }
    
    // When not running in debug mode ('-d'), the CWD gets set to '/' after
    // we're initialised. Convert relative paths to absolute ones.
    if (g_params.configfile[0] != '/')
    {
        static string absConf = cwd + '/' + g_params.configfile;
        g_params.configfile = absConf.c_str();
    }
    if (g_params.calibratedfile && g_params.calibratedfile[0] != '/')
    {
        static string absCal = cwd + '/' + g_params.calibratedfile;
        g_params.calibratedfile = absCal.c_str();
    }
    
    if ((g_params.major < 0 || g_params.minor < 0) && g_params.outdev)
    {
        // Get our major/minor device numbers from the specified device node
        struct stat s;
        if (stat(g_params.outdev, &s))
        {
            cerr << "couldn't stat " << g_params.outdev << "\n";
            return 1;
        }
        if (!S_ISCHR(s.st_mode))
        {
            cerr << g_params.outdev << " should be the output joystick - "
                 "should be a character device\n";
            return 1;
        }
        g_params.major = major(s.st_rdev);
        g_params.minor = minor(s.st_rdev);
    }
    if (g_params.major < 0 || g_params.minor < 0) 
    {
        cerr << "Please give device major/minor numbers to use - either "
                "-M/-m or -O options\n";
        return 1;
    }
    
    stickshift_clop.open    = stickshift_open;
    stickshift_clop.release = stickshift_release;
    stickshift_clop.read    = stickshift_read;
    stickshift_clop.ioctl   = stickshift_ioctl;
    stickshift_clop.poll    = stickshift_poll;
    stickshift_clop.init    = stickshift_init;
    stickshift_clop.destroy = stickshift_destroy;

help_fast_exit:
    
    string dev_name = str(boost::format("DEVNAME=stickshift%s")
                            % g_params.minor);
    
    const char *dev_info_argv[] = { dev_name.c_str() };
    ci.dev_major = g_params.major;
    ci.dev_minor = g_params.minor;
    ci.dev_info_argc = 1;
    ci.dev_info_argv = dev_info_argv;
    ci.flags = CUSE_UNRESTRICTED_IOCTL;

    return cuse_lowlevel_main(args.argc, args.argv, &ci, &stickshift_clop,
                              NULL);
}
