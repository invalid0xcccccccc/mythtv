// -*- Mode: c++ -*-

// POSIX headers
#include <thread>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#if !defined( USING_MINGW ) && !defined( _MSC_VER )
#include <poll.h>
#include <sys/ioctl.h>
#endif
#ifdef ANDROID
#include <sys/wait.h>
#endif

// Qt headers
#include <QString>
#include <QFile>

// MythTV headers
#include "ExternalStreamHandler.h"
#include "ExternalChannel.h"
//#include "ThreadedFileWriter.h"
#include "dtvsignalmonitor.h"
#include "streamlisteners.h"
#include "mpegstreamdata.h"
#include "cardutil.h"
#include "exitcodes.h"

#define LOC QString("ExternSH[%1](%2): ").arg(_inputid).arg(m_loc)

ExternIO::ExternIO(const QString & app,
                   const QStringList & args)
    : m_appin(-1), m_appout(-1), m_apperr(-1),
      m_pid(-1), m_bufsize(0), m_buffer(nullptr),
      m_status(&m_status_buf, QIODevice::ReadWrite),
      m_errcnt(0)

{
    m_app  = (app);

    if (!m_app.exists())
    {
        m_error = QString("ExternIO: '%1' does not exist.").arg(app);
        return;
    }
    if (!m_app.isReadable() || !m_app.isFile())
    {
        m_error = QString("ExternIO: '%1' is not readable.")
                  .arg(m_app.canonicalFilePath());
        return;
    }
    if (!m_app.isExecutable())
    {
        m_error = QString("ExternIO: '%1' is not executable.")
                  .arg(m_app.canonicalFilePath());
        return;
    }

    m_args = args;
    m_args.prepend(m_app.baseName());

    m_status.setString(&m_status_buf);
}

ExternIO::~ExternIO(void)
{
    close(m_appin);
    close(m_appout);
    close(m_apperr);

    // waitpid(m_pid, &status, 0);
    delete[] m_buffer;
}

bool ExternIO::Ready(int fd, int timeout, const QString & what)
{
#if !defined( USING_MINGW ) && !defined( _MSC_VER )
    struct pollfd m_poll[2];
    memset(m_poll, 0, sizeof(m_poll));

    m_poll[0].fd = fd;
    m_poll[0].events = POLLIN | POLLPRI;
    int ret = poll(m_poll, 1, timeout);

    if (m_poll[0].revents & POLLHUP)
    {
        m_error = what + " poll eof (POLLHUP)";
        return false;
    }
    else if (m_poll[0].revents & POLLNVAL)
    {
        LOG(VB_GENERAL, LOG_ERR, "poll error");
        return false;
    }
    if (m_poll[0].revents & POLLIN)
    {
        if (ret > 0)
            return true;

        if ((EOVERFLOW == errno))
            m_error = "poll overflow";
        return false;
    }
#endif // !defined( USING_MINGW ) && !defined( _MSC_VER )
    return false;
}

int ExternIO::Read(QByteArray & buffer, int maxlen, int timeout)
{
    if (Error())
    {
        LOG(VB_RECORD, LOG_ERR,
            QString("ExternIO::Read: already in error state: '%1'")
            .arg(m_error));
        return 0;
    }

    if (!Ready(m_appout, timeout, "data"))
        return 0;

    if (m_bufsize < maxlen)
    {
        m_bufsize = maxlen;
        delete m_buffer;
        m_buffer = new char[m_bufsize];
    }

    int len = read(m_appout, m_buffer, maxlen);

    if (len < 0)
    {
        if (errno == EAGAIN)
        {
            if (++m_errcnt > kMaxErrorCnt)
            {
                m_error = "Failed to read from External Recorder: " + ENO;
                LOG(VB_RECORD, LOG_WARNING,
                    "External Recorder not ready. Giving up.");
            }
            else
            {
                LOG(VB_RECORD, LOG_WARNING,
                    QString("External Recorder not ready. Will retry (%1/%2).")
                    .arg(m_errcnt).arg(kMaxErrorCnt));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        else
        {
            m_error = "Failed to read from External Recorder: " + ENO;
            LOG(VB_RECORD, LOG_ERR, m_error);
        }
    }
    else
        m_errcnt = 0;

    if (len == 0)
        return 0;

    buffer.append(m_buffer, len);

    LOG(VB_RECORD, LOG_DEBUG,
        QString("ExternIO::Read '%1' bytes, buffer size %2")
        .arg(len).arg(buffer.size()));

    return len;
}

QString ExternIO::GetStatus(int timeout)
{
    if (Error())
    {
        LOG(VB_RECORD, LOG_ERR,
            QString("ExternIO::GetStatus: already in error state: '%1'")
            .arg(m_error));
        return QByteArray();
    }

    int waitfor = m_status.atEnd() ? timeout : 0;
    if (Ready(m_apperr, waitfor, "status"))
    {
        char buffer[2048];
        int len = read(m_apperr, buffer, 2048);
        m_status << QString::fromLatin1(buffer, len);
    }

    if (m_status.atEnd())
        return QByteArray();

    QString msg = m_status.readLine();

    LOG(VB_RECORD, LOG_DEBUG, QString("ExternIO::GetStatus '%1'")
        .arg(msg));

    return msg;
}

int ExternIO::Write(const QByteArray & buffer)
{
    if (Error())
    {
        LOG(VB_RECORD, LOG_ERR,
            QString("ExternIO::Write: already in error state: '%1'")
            .arg(m_error));
        return -1;
    }

    LOG(VB_RECORD, LOG_DEBUG, QString("ExternIO::Write('%1')")
        .arg(QString(buffer)));

    int len = write(m_appin, buffer.constData(), buffer.size());
    if (len != buffer.size())
    {
        if (len > 0)
        {
            LOG(VB_RECORD, LOG_WARNING,
                QString("ExternIO::Write: only wrote %1 of %2 bytes '%3'")
                .arg(len).arg(buffer.size()).arg(QString(buffer)));
        }
        else
        {
            m_error = QString("ExternIO: Failed to write '%1' to app's stdin: ")
                      .arg(QString(buffer)) + ENO;
            return -1;
        }
    }

    return len;
}

bool ExternIO::Run(void)
{
    LOG(VB_RECORD, LOG_INFO, QString("ExternIO::Run()"));

    Fork();
    GetStatus(10);

    return true;
}

/* Return true if the process is not, or is no longer running */
bool ExternIO::KillIfRunning(const QString & cmd)
{
#if CONFIG_DARWIN || (__FreeBSD__) || defined(__OpenBSD__)
    Q_UNUSED(cmd);
    return false;
#elif defined USING_MINGW
    Q_UNUSED(cmd);
    return false;
#elif defined( _MSC_VER )
    Q_UNUSED(cmd);
    return false;
#else
    QString grp = QString("pgrep -x -f -- \"%1\" 2>&1 > /dev/null").arg(cmd);
    QString kil = QString("pkill --signal 15 -x -f -- \"%1\" 2>&1 > /dev/null")
                  .arg(cmd);
    int res_grp, res_kil;

    res_grp = system(grp.toUtf8().constData());
    if (WEXITSTATUS(res_grp) == 1)
    {
        LOG(VB_RECORD, LOG_DEBUG, QString("'%1' not running.").arg(cmd));
        return true;
    }

    LOG(VB_RECORD, LOG_WARNING, QString("'%1' already running, killing...")
        .arg(cmd));
    res_kil = system(kil.toUtf8().constData());
    if (WEXITSTATUS(res_kil) == 1)
        LOG(VB_GENERAL, LOG_WARNING, QString("'%1' failed: %2")
            .arg(kil).arg(ENO));

    res_grp = system(grp.toUtf8().constData());
    if (WEXITSTATUS(res_grp) == 1)
    {
        LOG(WEXITSTATUS(res_kil) == 0 ? VB_RECORD : VB_GENERAL, LOG_WARNING,
            QString("'%1' terminated.").arg(cmd));
        return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    kil = QString("pkill --signal 9 -x -f \"%1\" 2>&1 > /dev/null").arg(cmd);
    res_kil = system(kil.toUtf8().constData());
    if (WEXITSTATUS(res_kil) > 0)
        LOG(VB_GENERAL, LOG_WARNING, QString("'%1' failed: %2")
            .arg(kil).arg(ENO));

    res_grp = system(grp.toUtf8().constData());
    LOG(WEXITSTATUS(res_kil) == 0 ? VB_RECORD : VB_GENERAL, LOG_WARNING,
        QString("'%1' %2.").arg(cmd)
        .arg(WEXITSTATUS(res_grp) == 0 ? "sill running" : "terminated"));

    return (WEXITSTATUS(res_grp) != 0);
#endif
}

void ExternIO::Fork(void)
{
#if !defined( USING_MINGW ) && !defined( _MSC_VER )
    if (Error())
    {
        LOG(VB_RECORD, LOG_INFO, QString("ExternIO in bad state: '%1'")
            .arg(m_error));
        return;
    }

    QString full_command = QString("%1").arg(m_args.join(" "));

    if (!KillIfRunning(full_command))
    {
        // Give it one more chance.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!KillIfRunning(full_command))
        {
            m_error = QString("Unable to kill existing '%1'.")
                      .arg(full_command);
            LOG(VB_GENERAL, LOG_ERR, m_error);
            return;
        }
    }


    LOG(VB_RECORD, LOG_INFO, QString("ExternIO::Fork '%1'").arg(full_command));

    int in[2]  = {-1, -1};
    int out[2] = {-1, -1};
    int err[2] = {-1, -1};

    if (pipe(in) < 0)
    {
        m_error = "pipe(in) failed: " + ENO;
        return;
    }
    if (pipe(out) < 0)
    {
        m_error = "pipe(out) failed: " + ENO;
        close(in[0]);
        close(in[1]);
        return;
    }
    if (pipe(err) < 0)
    {
        m_error = "pipe(err) failed: " + ENO;
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        return;
    }

    m_pid = fork();
    if (m_pid < 0)
    {
        // Failed
        m_error = "fork() failed: " + ENO;
        return;
    }
    if (m_pid > 0)
    {
        // Parent
        close(in[0]);
        close(out[1]);
        close(err[1]);
        m_appin  = in[1];
        m_appout = out[0];
        m_apperr = err[0];

        bool error = false;
        error = (fcntl(m_appin,  F_SETFL, O_NONBLOCK) == -1);
        error |= (fcntl(m_appout, F_SETFL, O_NONBLOCK) == -1);
        error |= (fcntl(m_apperr, F_SETFL, O_NONBLOCK) == -1);

        if (error)
        {
            LOG(VB_GENERAL, LOG_WARNING,
                "ExternIO::Fork(): Failed to set O_NONBLOCK for FD: " + ENO);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            _exit(GENERIC_EXIT_PIPE_FAILURE);
        }

        LOG(VB_RECORD, LOG_INFO, "Spawned");
        return;
    }

    // Child
    close(in[1]);
    close(out[0]);
    close(err[0]);
    if (dup2( in[0], 0) < 0)
    {
        std::cerr << "dup2(stdin) failed: " << strerror(errno);
        _exit(GENERIC_EXIT_PIPE_FAILURE);
    }
    else if (dup2(out[1], 1) < 0)
    {
        std::cerr << "dup2(stdout) failed: " << strerror(errno);
        _exit(GENERIC_EXIT_PIPE_FAILURE);
    }
    else if (dup2(err[1], 2) < 0)
    {
        std::cerr << "dup2(stderr) failed: " << strerror(errno);
        _exit(GENERIC_EXIT_PIPE_FAILURE);
    }

    /* Close all open file descriptors except stdin/stdout/stderr */
    for (int i = sysconf(_SC_OPEN_MAX) - 1; i > 2; --i)
        close(i);

    /* Set the process group id to be the same as the pid of this
     * child process.  This ensures that any subprocesses launched by this
     * process can be killed along with the process itself. */
    if (setpgid(0,0) < 0)
    {
        std::cerr << "ExternIO: "
             << "setpgid() failed: "
             << strerror(errno) << endl;
    }

    /* run command */
    char *command = strdup(m_app.canonicalFilePath()
                                 .toUtf8().constData());
    char **arguments;
    int    len;

    // Copy QStringList to char**
    arguments = new char*[m_args.size() + 1];
    for (int i = 0; i < m_args.size(); ++i)
    {
        len = m_args[i].size() + 1;
        arguments[i] = new char[len];
        memcpy(arguments[i], m_args[i].toStdString().c_str(), len);
    }
    arguments[m_args.size()] = nullptr;

    if (execv(command, arguments) < 0)
    {
        // Can't use LOG due to locking fun.
        std::cerr << "ExternIO: "
             << "execv() failed: "
             << strerror(errno) << endl;
    }
    else
    {
        std::cerr << "ExternIO: "
                  << "execv() should not be here?: "
                  << strerror(errno) << endl;
    }

#endif // !defined( USING_MINGW ) && !defined( _MSC_VER )

    /* Failed to exec */
    _exit(GENERIC_EXIT_DAEMONIZING_ERROR); // this exit is ok
}


QMap<int, ExternalStreamHandler*> ExternalStreamHandler::m_handlers;
QMap<int, uint>                   ExternalStreamHandler::m_handlers_refcnt;
QMutex                            ExternalStreamHandler::m_handlers_lock;

ExternalStreamHandler *ExternalStreamHandler::Get(const QString &devname,
                                                  int inputid, int majorid)
{
    QMutexLocker locker(&m_handlers_lock);

    QMap<int, ExternalStreamHandler*>::iterator it = m_handlers.find(majorid);

    if (it == m_handlers.end())
    {
        ExternalStreamHandler *newhandler =
            new ExternalStreamHandler(devname, inputid, majorid);
        m_handlers[majorid] = newhandler;
        m_handlers_refcnt[majorid] = 1;

        LOG(VB_RECORD, LOG_INFO,
            QString("ExternSH[%1]: Creating new stream handler %2 for %3")
            .arg(inputid).arg(majorid).arg(devname));
    }
    else
    {
        m_handlers_refcnt[majorid]++;
        uint rcount = m_handlers_refcnt[majorid];
        LOG(VB_RECORD, LOG_INFO,
            QString("ExternSH[%1]: Using existing stream handler for %2")
            .arg(inputid).arg(majorid) + QString(" (%1 in use)").arg(rcount));
    }

    return m_handlers[majorid];
}

void ExternalStreamHandler::Return(ExternalStreamHandler * & ref,
                                   int inputid)
{
    QMutexLocker locker(&m_handlers_lock);

    QString devname = ref->_device;
    int majorid = ref->m_majorid;

    QMap<int, uint>::iterator rit = m_handlers_refcnt.find(majorid);
    if (rit == m_handlers_refcnt.end())
        return;

    QMap<int, ExternalStreamHandler*>::iterator it =
        m_handlers.find(majorid);

    LOG(VB_RECORD, LOG_INFO, QString("ExternSH[%1]: Return %2 in use %3")
        .arg(inputid).arg(majorid).arg(*rit));

    if (*rit > 1)
    {
        ref = nullptr;
        --(*rit);
        return;
    }

    if ((it != m_handlers.end()) && (*it == ref))
    {
        LOG(VB_RECORD, LOG_INFO, QString("ExternSH[%1]: Closing handler for %2")
            .arg(inputid).arg(majorid));
        delete *it;
        m_handlers.erase(it);
    }
    else
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("ExternSH[%1]: Error: Couldn't find handler for %2")
            .arg(inputid).arg(majorid));
    }

    m_handlers_refcnt.erase(rit);
    ref = nullptr;
}

/*
  ExternalStreamHandler
 */

ExternalStreamHandler::ExternalStreamHandler(const QString & path,
                                             int inputid,
                                             int majorid)
    : StreamHandler(path, inputid)
    , m_loc(_device)
    , m_majorid(majorid)
    , m_IO(nullptr)
    , m_tsopen(false)
    , m_io_errcnt(0)
    , m_poll_mode(false)
    , m_apiVersion(1)
    , m_serialNo(0)
    , m_replay(false)
    , m_xon(false)
{
    setObjectName("ExternSH");

    m_args = path.split(' ',QString::SkipEmptyParts) +
             logPropagateArgs.split(' ', QString::SkipEmptyParts);
    m_app = m_args.first();
    m_args.removeFirst();

    // Pass one (and only one) 'quiet'
    if (!m_args.contains("--quiet") && !m_args.contains("-q"))
        m_args << "--quiet";

    m_args << "--inputid" << QString::number(majorid);
    LOG(VB_RECORD, LOG_INFO, LOC + QString("args \"%1\"")
        .arg(m_args.join(" ")));

    if (!OpenApp())
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            QString("Failed to start %1").arg(_device));
    }
}

int ExternalStreamHandler::StreamingCount(void) const
{
    return m_streaming_cnt.loadAcquire();
}

void ExternalStreamHandler::run(void)
{
    QString    cmd;
    QString    result;
    QString    ready_cmd;
    QByteArray buffer;
    int        sz;
    uint       len, read_len;
    uint       restart_cnt = 0;
    MythTimer  status_timer;
    MythTimer  nodata_timer;

    bool       good_data = false;
    uint       data_proc_err = 0;
    uint       data_short_err = 0;

    if (!m_IO)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            QString("%1 is not running.").arg(_device));
    }

    status_timer.start();

    RunProlog();

    LOG(VB_RECORD, LOG_INFO, LOC + "run(): begin");

    SetRunning(true, true, false);

    if (m_poll_mode)
        ready_cmd = "SendBytes";
    else
        ready_cmd = "XON";

    uint remainder = 0;
    while (_running_desired && !_error)
    {
        if (!IsTSOpen())
        {
            LOG(VB_RECORD, LOG_WARNING, LOC + "TS not open yet.");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (StreamingCount() == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        UpdateFiltersFromStreamData();

        if (!m_xon || m_poll_mode)
        {
            if (buffer.size() > TOO_FAST_SIZE)
            {
                LOG(VB_RECORD, LOG_WARNING, LOC +
                    "Internal buffer too full to accept more data from "
                    "external application.");
            }
            else
            {
                if (!ProcessCommand(ready_cmd, result))
                {
                    if (result.startsWith("ERR"))
                    {
                        LOG(VB_GENERAL, LOG_ERR, LOC +
                            QString("Aborting: %1 -> %2")
                            .arg(ready_cmd).arg(result));
                        _error = true;
                        continue;
                    }

                    if (restart_cnt++)
                        std::this_thread::sleep_for(std::chrono::seconds(20));
                    if (!RestartStream())
                    {
                        LOG(VB_RECORD, LOG_ERR, LOC +
                            "Failed to restart stream.");
                        _error = true;
                    }
                    continue;
                }
                m_xon = true;
            }
        }

        if (m_xon)
        {
            if (status_timer.elapsed() >= 2000)
            {
                // Since we may never need to send the XOFF
                // command, occationally check to see if the
                // External recorder needs to report an issue.
                if (CheckForError())
                {
                    if (restart_cnt++)
                        std::this_thread::sleep_for(std::chrono::seconds(20));
                    if (!RestartStream())
                    {
                        LOG(VB_RECORD, LOG_ERR, LOC + "Failed to restart stream.");
                        _error = true;
                    }
                    continue;
                }

                status_timer.restart();
            }

            if (buffer.size() > TOO_FAST_SIZE)
            {
                if (!m_poll_mode)
                {
                    // Data is comming a little too fast, so XOFF
                    // to give us time to process it.
                    if (!ProcessCommand(QString("XOFF"), result))
                    {
                        if (result.startsWith("ERR"))
                        {
                            LOG(VB_GENERAL, LOG_ERR, LOC +
                                QString("Aborting: XOFF -> %2")
                                .arg(result));
                            _error = true;
                        }
                    }
                    m_xon = false;
                }
            }

            if (m_IO && (sz = PACKET_SIZE - remainder) > 0)
                read_len = m_IO->Read(buffer, sz, 100);
            else
                read_len = 0;
        }
        else
            read_len = 0;

        if (read_len == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(50));

            if (!nodata_timer.isRunning())
                nodata_timer.start();
            else
            {
                if (nodata_timer.elapsed() >= 50000)
                {
                    LOG(VB_GENERAL, LOG_WARNING, LOC +
                        "No data for 50 seconds, Restarting stream.");
                    if (!RestartStream())
                    {
                        LOG(VB_RECORD, LOG_ERR, LOC + "Failed to restart stream.");
                        _error = true;
                    }
                    nodata_timer.stop();
                    continue;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        else
        {
            nodata_timer.stop();
            restart_cnt = 0;
        }

        if (m_IO == nullptr)
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "I/O thread has disappeared!");
            _error = true;
            break;
        }
        if (m_IO->Error())
        {
            LOG(VB_GENERAL, LOG_ERR, LOC +
                QString("Fatal Error from External Recorder: %1")
                .arg(m_IO->ErrorString()));
            CloseApp();
            _error = true;
            break;
        }

        len = remainder = buffer.size();

        if (len == 0)
            continue;

        if (len < TS_PACKET_SIZE)
        {
            if (m_xon && data_short_err++ == 0)
                LOG(VB_RECORD, LOG_INFO, LOC + "Waiting for a full TS packet.");
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }
        else if (data_short_err)
        {
            if (data_short_err > 1)
            {
                LOG(VB_RECORD, LOG_INFO, LOC +
                    QString("Waited for a full TS packet %1 times.")
                    .arg(data_short_err));
            }
            data_short_err = 0;
        }

        if (!m_stream_lock.tryLock())
            continue;

        if (!_listener_lock.tryLock())
            continue;

        StreamDataList::const_iterator sit = _stream_data_list.begin();
        for (; sit != _stream_data_list.end(); ++sit)
            remainder = sit.key()->ProcessData
                        (reinterpret_cast<const uint8_t *>
                         (buffer.constData()), buffer.size());

        _listener_lock.unlock();

        if (m_replay)
        {
            m_replay_buffer += buffer.left(len - remainder);
            if (m_replay_buffer.size() > (50 * PACKET_SIZE))
            {
                m_replay_buffer.remove(0, len - remainder);
                LOG(VB_RECORD, LOG_WARNING, LOC +
                    QString("Replay size truncated to %1 bytes")
                    .arg(m_replay_buffer.size()));
            }
        }

        m_stream_lock.unlock();

        if (remainder == 0)
        {
            buffer.clear();
            good_data = len;
        }
        else if (len > remainder) // leftover bytes
        {
            buffer.remove(0, len - remainder);
            good_data = len;
        }
        else if (len == remainder)
            good_data = false;

        if (good_data)
        {
            if (data_proc_err)
            {
                if (data_proc_err > 1)
                {
                    LOG(VB_RECORD, LOG_WARNING, LOC +
                        QString("Failed to process the data received %1 times.")
                        .arg(data_proc_err));
                }
                data_proc_err = 0;
            }
        }
        else
        {
            if (data_proc_err++ == 0)
            {
                LOG(VB_RECORD, LOG_WARNING, LOC +
                    "Failed to process the data received");
            }
        }
    }

    LOG(VB_RECORD, LOG_INFO, LOC + "run(): " +
        QString("%1 shutdown").arg(_error ? "Error" : "Normal"));

    RemoveAllPIDFilters();
    SetRunning(false, true, false);

    LOG(VB_RECORD, LOG_INFO, LOC + "run(): " + "end");

    RunEpilog();
}

bool ExternalStreamHandler::SetAPIVersion(void)
{
    QString result;

    if (ProcessCommand("APIVersion?", result, 10000))
    {
        QStringList tokens = result.split(':', QString::SkipEmptyParts);

        if (tokens.size() > 1)
            m_apiVersion = tokens[1].toUInt();
        m_apiVersion = min(m_apiVersion, static_cast<int>(MAX_API_VERSION));
        if (m_apiVersion < 1)
        {
            LOG(VB_RECORD, LOG_ERR, LOC +
                QString("Bad response to 'APIVersion?' - '%1'. "
                        "Expecting 1 or 2").arg(result));
            m_apiVersion = 1;
        }

        ProcessCommand(QString("APIVersion:%1").arg(m_apiVersion), result);
        return true;
    }

    return false;
}

QString ExternalStreamHandler::UpdateDescription(void)
{
    if (m_apiVersion > 1)
    {
        QString result;

        if (ProcessCommand("Description?", result))
            m_loc = result.mid(3);
        else
            m_loc = _device;
    }

    return m_loc;
}

bool ExternalStreamHandler::OpenApp(void)
{
    {
        QMutexLocker locker(&m_IO_lock);

        if (m_IO)
        {
            LOG(VB_RECORD, LOG_WARNING, LOC + "OpenApp: already open!");
            return true;
        }

        m_IO = new ExternIO(m_app, m_args);

        if (m_IO == nullptr)
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "ExternIO failed: " + ENO);
            _error = true;
        }
        else
        {
            LOG(VB_RECORD, LOG_INFO, LOC + QString("Spawn '%1'").arg(_device));
            m_IO->Run();
            if (m_IO->Error())
            {
                LOG(VB_GENERAL, LOG_ERR,
                    "Failed to start External Recorder: " + m_IO->ErrorString());
                delete m_IO;
                m_IO = nullptr;
                _error = true;
                return false;
            }
        }
    }

    QString result;

    if (!SetAPIVersion())
    {
        // Try again using API version 2
        m_apiVersion = 2;
        if (!SetAPIVersion())
            m_apiVersion = 1;
    }

    if (!IsAppOpen())
    {
        LOG(VB_RECORD, LOG_ERR, LOC + "Application is not responding.");
        _error = true;
        return false;
    }

    UpdateDescription();

    // Gather capabilities
    if (!ProcessCommand("HasTuner?", result))
    {
        LOG(VB_RECORD, LOG_ERR, LOC +
            QString("Bad response to 'HasTuner?' - '%1'").arg(result));
        _error = true;
        return false;
    }
    m_hasTuner = result.startsWith("OK:Yes");

    if (!ProcessCommand("HasPictureAttributes?", result))
    {
        LOG(VB_RECORD, LOG_ERR, LOC +
            QString("Bad response to 'HasPictureAttributes?' - '%1'")
            .arg(result));
        _error = true;
        return false;
    }
    m_hasPictureAttributes = result.startsWith("OK:Yes");

    /* Operate in "poll" or "xon/xoff" mode */
    m_poll_mode = ProcessCommand("FlowControl?", result) &&
                  result.startsWith("OK:Poll");

    LOG(VB_RECORD, LOG_INFO, LOC + "App opened successfully");
    LOG(VB_RECORD, LOG_INFO, LOC +
        QString("Capabilities: tuner(%1) "
                "Picture attributes(%2) "
                "Flow control(%3)")
        .arg(m_hasTuner ? "yes" : "no")
        .arg(m_hasPictureAttributes ? "yes" : "no")
        .arg(m_poll_mode ? "Polling" : "XON/XOFF")
        );

    /* Let the external app know how many bytes will read without blocking */
    ProcessCommand(QString("BlockSize:%1").arg(PACKET_SIZE), result);

    return true;
}

bool ExternalStreamHandler::IsAppOpen(void)
{
    if (m_IO == nullptr)
    {
        LOG(VB_RECORD, LOG_WARNING, LOC +
            "WARNING: Unable to communicate with external app.");
        return false;
    }

    QString result;
    return ProcessCommand("Version?", result, 10000);
}

bool ExternalStreamHandler::IsTSOpen(void)
{
    if (m_tsopen)
        return true;

    QString result;

    if (!ProcessCommand("IsOpen?", result))
        return false;

    m_tsopen = true;
    return m_tsopen;
}

void ExternalStreamHandler::CloseApp(void)
{
    m_IO_lock.lock();
    if (m_IO)
    {
        QString result;

        LOG(VB_RECORD, LOG_INFO, LOC + "CloseRecorder");
        m_IO_lock.unlock();
        ProcessCommand("CloseRecorder", result, 10000);
        m_IO_lock.lock();

        if (!result.startsWith("OK"))
        {
            LOG(VB_RECORD, LOG_INFO, LOC +
                "CloseRecorder failed, sending kill.");

            QString full_command = QString("%1").arg(m_args.join(" "));

            if (!m_IO->KillIfRunning(full_command))
            {
                // Give it one more chance.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (!m_IO->KillIfRunning(full_command))
                {
                    LOG(VB_GENERAL, LOG_ERR,
                        QString("Unable to kill existing '%1'.")
                        .arg(full_command));
                    return;
                }
            }
        }
        delete m_IO;
        m_IO = nullptr;
    }
    m_IO_lock.unlock();
}

bool ExternalStreamHandler::RestartStream(void)
{
    bool streaming = (StreamingCount() > 0);

    LOG(VB_RECORD, LOG_INFO, LOC + "Restarting stream.");

    if (streaming)
        StopStreaming();

    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (streaming)
        return StartStreaming();

    return true;
}

void ExternalStreamHandler::ReplayStream(void)
{
    if (m_replay)
    {
        QString    result;

        // Let the external app know that we could be busy for a little while
        if (!m_poll_mode)
        {
            ProcessCommand(QString("XOFF"), result);
            m_xon = false;
        }

        /* If the input is not a 'broadcast' it may only have one
         * copy of the SPS right at the beginning of the stream,
         * so make sure we don't miss it!
         */
        QMutexLocker listen_lock(&_listener_lock);

        if (!_stream_data_list.empty())
        {
            StreamDataList::const_iterator sit = _stream_data_list.begin();
            for (; sit != _stream_data_list.end(); ++sit)
                sit.key()->ProcessData(reinterpret_cast<const uint8_t *>
                                       (m_replay_buffer.constData()),
                                       m_replay_buffer.size());
        }
        LOG(VB_RECORD, LOG_INFO, LOC + QString("Replayed %1 bytes")
            .arg(m_replay_buffer.size()));
        m_replay_buffer.clear();
        m_replay = false;

        // Let the external app know that we are ready
        if (!m_poll_mode)
        {
            if (ProcessCommand(QString("XON"), result))
                m_xon = true;
        }
    }
}

bool ExternalStreamHandler::StartStreaming(void)
{
    QString result;

    QMutexLocker locker(&m_stream_lock);

    UpdateDescription();

    LOG(VB_RECORD, LOG_INFO, LOC +
        QString("StartStreaming with %1 current listeners")
        .arg(StreamingCount()));

    if (!IsAppOpen())
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "External Recorder not started.");
        return false;
    }

    if (StreamingCount() == 0)
    {
        if (!ProcessCommand("StartStreaming", result, 10000))
        {
            LogLevel_t level = LOG_ERR;
            if (result.toLower().startsWith("warn"))
                level = LOG_WARNING;
            else
                _error = true;

            LOG(VB_GENERAL, level, LOC + QString("StartStreaming failed: '%1'")
                .arg(result));

            return false;
        }

        LOG(VB_RECORD, LOG_INFO, LOC + "Streaming started");
    }
    else
        LOG(VB_RECORD, LOG_INFO, LOC + "Already streaming");

    m_streaming_cnt.ref();

    LOG(VB_RECORD, LOG_INFO, LOC +
        QString("StartStreaming %1 listeners")
        .arg(StreamingCount()));

    return true;
}

bool ExternalStreamHandler::StopStreaming(void)
{
    QString result;

    QMutexLocker locker(&m_stream_lock);

    LOG(VB_RECORD, LOG_INFO, LOC +
        QString("StopStreaming %1 listeners")
        .arg(StreamingCount()));

    if (StreamingCount() == 0)
    {
        LOG(VB_RECORD, LOG_INFO, LOC +
            "StopStreaming requested, but we are not streaming!");
        return true;
    }

    if (m_streaming_cnt.deref())
    {
        LOG(VB_RECORD, LOG_INFO, LOC +
            QString("StopStreaming delayed, still have %1 listeners")
            .arg(StreamingCount()));
        return true;
    }

    LOG(VB_RECORD, LOG_INFO, LOC + "StopStreaming");

    if (!m_poll_mode && m_xon)
    {
        QString result;
        ProcessCommand(QString("XOFF"), result);
        m_xon = false;
    }

    if (!IsAppOpen())
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "External Recorder not started.");
        return false;
    }

    if (!ProcessCommand("StopStreaming", result, 6000))
    {
        LogLevel_t level = LOG_ERR;
        if (result.toLower().startsWith("warn"))
            level = LOG_WARNING;
        else
            _error = true;

        LOG(VB_GENERAL, level, LOC + QString("StopStreaming: '%1'")
            .arg(result));

        return false;
    }

    PurgeBuffer();
    LOG(VB_RECORD, LOG_INFO, LOC + "Streaming stopped");

    return true;
}

bool ExternalStreamHandler::ProcessCommand(const QString & cmd,
                                           QString & result, int timeout,
                                           uint retry_cnt)
{
    QMutexLocker locker(&m_process_lock);

    if (m_apiVersion == 2)
        return ProcessVer2(cmd, result, timeout, retry_cnt);
    else if (m_apiVersion == 1)
        return ProcessVer1(cmd, result, timeout, retry_cnt);

    LOG(VB_RECORD, LOG_ERR, LOC +
        QString("Invalid API version %1.  Expected 1 or 2").arg(m_apiVersion));
    return false;
}

bool ExternalStreamHandler::ProcessVer1(const QString & cmd,
                                        QString & result, int timeout,
                                        uint retry_cnt)
{
    bool okay;

    LOG(VB_RECORD, LOG_DEBUG, LOC + QString("ProcessVer1('%1')")
        .arg(cmd));

    for (uint cnt = 0; cnt < retry_cnt; ++cnt)
    {
        QMutexLocker locker(&m_IO_lock);

        if (!m_IO)
        {
            LOG(VB_RECORD, LOG_ERR, LOC + "External I/O not ready!");
            return false;
        }

        QByteArray buf(cmd.toUtf8(), cmd.size());
        buf += '\n';

        if (m_IO->Error())
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "External Recorder in bad state: " +
                m_IO->ErrorString());
            return false;
        }

        /* Try to keep in sync, if External app was too slow in responding
         * to previous query, consume the response before sending new query */
        m_IO->GetStatus(0);

        /* Send new query */
        m_IO->Write(buf);

        MythTimer timer(MythTimer::kStartRunning);
        while (timer.elapsed() < timeout)
        {
            result = m_IO->GetStatus(timeout);
            if (m_IO->Error())
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "Failed to read from External Recorder: " +
                    m_IO->ErrorString());
                    _error = true;
                return false;
            }

            // Out-of-band error message
            if (result.startsWith("STATUS:ERR") ||
                result.startsWith("0:STATUS:ERR"))
            {
                LOG(VB_RECORD, LOG_ERR, LOC + result);
                result.remove(0, result.indexOf(":ERR") + 1);
                return false;
            }
            // STATUS message are "out of band".
            // Ignore them while waiting for a responds to a command
            if (!result.startsWith("STATUS") && !result.startsWith("0:STATUS"))
                break;
            LOG(VB_RECORD, LOG_INFO, LOC +
                QString("Ignoring response '%1'").arg(result));
        }

        if (result.size() < 1)
        {
            LOG(VB_GENERAL, LOG_WARNING, LOC +
                QString("External Recorder did not respond to '%1'").arg(cmd));
        }
        else
        {
            okay = result.startsWith("OK");
            if (okay || result.startsWith("WARN") || result.startsWith("ERR"))
            {
                LogLevel_t level = LOG_INFO;

                m_io_errcnt = 0;
                if (!okay)
                    level = LOG_WARNING;
                else if (cmd.startsWith("SendBytes"))
                    level = LOG_DEBUG;

                LOG(VB_RECORD, level,
                    LOC + QString("ProcessCommand('%1') = '%2' took %3ms %4")
                    .arg(cmd).arg(result)
                    .arg(timer.elapsed())
                    .arg(okay ? "" : "<-- NOTE"));

                return okay;
            }
            else
                LOG(VB_GENERAL, LOG_WARNING, LOC +
                    QString("External Recorder invalid response to '%1': '%2'")
                    .arg(cmd).arg(result));
        }

        if (++m_io_errcnt > 10)
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "Too many I/O errors.");
            _error = true;
            break;
        }
    }

    return false;
}

bool ExternalStreamHandler::ProcessVer2(const QString & command,
                                        QString & result, int timeout,
                                        uint retry_cnt)
{
    bool    okay;
    bool    err;
    QString status;
    QString raw;

    for (uint cnt = 0; cnt < retry_cnt; ++cnt)
    {
        QString cmd = QString("%1:%2").arg(++m_serialNo).arg(command);

        LOG(VB_RECORD, LOG_DEBUG, LOC + QString("ProcessVer2('%1') serial(%2)")
            .arg(cmd).arg(m_serialNo));

        QMutexLocker locker(&m_IO_lock);

        if (!m_IO)
        {
            LOG(VB_RECORD, LOG_ERR, LOC + "External I/O not ready!");
            return false;
        }

        QByteArray buf(cmd.toUtf8(), cmd.size());
        buf += '\n';

        if (m_IO->Error())
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "External Recorder in bad state: " +
                m_IO->ErrorString());
            return false;
        }

        /* Send query */
        m_IO->Write(buf);

        QStringList tokens;

        MythTimer timer(MythTimer::kStartRunning);
        while (timer.elapsed() < timeout)
        {
            result = m_IO->GetStatus(timeout);
            if (m_IO->Error())
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "Failed to read from External Recorder: " +
                    m_IO->ErrorString());
                    _error = true;
                return false;
            }

            if (!result.isEmpty())
            {
                raw = result;
                tokens = result.split(':', QString::SkipEmptyParts);

                // Look for result with the serial number of this query
                if (tokens.size() > 1 && tokens[0].toUInt() >= m_serialNo)
                    break;

                /* Other messages are "out of band" */

                // Check for error message missing serial#
                if (tokens[0].startsWith("ERR"))
                    break;

                // Remove serial#
                tokens.removeFirst();
                result = tokens.join(':');
                err = (tokens.size() > 1 && tokens[1].startsWith("ERR"));
                LOG(VB_RECORD, (err ? LOG_WARNING : LOG_INFO), LOC + raw);
                if (err)
                {
                    // Remove "STATUS"
                    tokens.removeFirst();
                    result = tokens.join(':');
                    return false;
                }
            }
        }

        if (timer.elapsed() >= timeout)
        {
            LOG(VB_RECORD, LOG_ERR, LOC +
                QString("ProcessVer2: Giving up waiting for response for "
                        "command '%2'").arg(cmd));
        }
        else if (tokens.size() < 2)
        {
            LOG(VB_RECORD, LOG_ERR, LOC +
                QString("Did not receive a valid response "
                        "for command '%1', received '%2'").arg(cmd).arg(result));
        }
        else if (tokens[0].toUInt() > m_serialNo)
        {
            LOG(VB_RECORD, LOG_ERR, LOC +
                QString("ProcessVer2: Looking for serial no %1, "
                        "but received %2 for command '%2'")
                .arg(m_serialNo).arg(tokens[0]).arg(cmd));
        }
        else
        {
            tokens.removeFirst();
            status = tokens[0].trimmed();
            result = tokens.join(':');

            okay = (status == "OK");
            if (okay || status.startsWith("WARN") || status.startsWith("ERR"))
            {
                LogLevel_t level = LOG_INFO;

                m_io_errcnt = 0;
                if (!okay)
                    level = LOG_WARNING;
                else if (command.startsWith("SendBytes"))
                    level = LOG_DEBUG;

                LOG(VB_RECORD, level,
                    LOC + QString("ProcessV2('%1') = '%2' took %3ms %4")
                    .arg(cmd).arg(result).arg(timer.elapsed())
                    .arg(okay ? "" : "<-- NOTE"));

                return okay;
            }
            else
                LOG(VB_GENERAL, LOG_WARNING, LOC +
                    QString("External Recorder invalid response to '%1': '%2'")
                    .arg(cmd).arg(result));
        }

        if (++m_io_errcnt > 10)
        {
            LOG(VB_GENERAL, LOG_ERR, LOC + "Too many I/O errors.");
            _error = true;
            break;
        }
    }

    return false;
}

bool ExternalStreamHandler::CheckForError(void)
{
    QString result;
    bool    err = false;

    QMutexLocker locker(&m_IO_lock);

    if (!m_IO)
    {
        LOG(VB_RECORD, LOG_ERR, LOC + "External I/O not ready!");
        return true;
    }

    if (m_IO->Error())
    {
        LOG(VB_GENERAL, LOG_ERR, "External Recorder in bad state: " +
            m_IO->ErrorString());
        return true;
    }

    do
    {
        result = m_IO->GetStatus(0);
        if (!result.isEmpty())
        {
            if (m_apiVersion > 1)
            {
                QStringList tokens = result.split(':', QString::SkipEmptyParts);

                tokens.removeFirst();
                result = tokens.join(':');
                for (int idx = 1; idx < tokens.size(); ++idx)
                    err |= tokens[idx].startsWith("ERR");
            }
            else
                err |= result.startsWith("STATUS:ERR");

            LOG(VB_RECORD, (err ? LOG_WARNING : LOG_INFO), LOC + result);
        }
    }
    while (!result.isEmpty());

    return err;
}

void ExternalStreamHandler::PurgeBuffer(void)
{
    if (m_IO)
    {
        QByteArray buffer;
        m_IO->Read(buffer, PACKET_SIZE, 1);
        m_IO->GetStatus(1);
    }
}

void ExternalStreamHandler::PriorityEvent(int /*fd*/)
{
    // TODO report on buffer overruns, etc.
}
