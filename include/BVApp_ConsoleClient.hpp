#pragma once
#include <iostream>
#include <string>
#include <boost/asio.hpp>
#include <termios.h>
#include <unistd.h>
#include <optional>
#include <limits>
#include <algorithm>
#include <cctype>
#include "BVApp.hpp"
#include "BVComponent.hpp"
#include "BVLoggable.hpp"
#include "api_common.h"

/*
    BVApp_ConsoleClient_Bonjour functions as a console application of LocalChat.
    Define as a function object, because it should be run in a different thread?

    Maybe it should be just a simple application with a ">> " prompt.
    User types: "List" -> prints browsed clients
    User types: "Message XXX MMMMMMM" -> sends message
    etc...
    As simple as it gets.
    It does however, browse continuously for new services.
    It can use (LISTENFORMESSAGE) which basically tells us that the client blocks on read from
    stdin OR it can create a separate thread on which it listens for message and updates "screen" =>
    prints ~70 new lines and messages and prompt etc.
    Different screens - main screen for available hosts and then separate screen for each host
    Maybe separate object that does I/O operation.
    In form of a dispatcher that is a separate thread, but THE ONLY thread that operates on stdout
    It has its queue and waits for events.
*/

enum class BVConsoleActionType
{
    BVCONSOLEACTION_SENDMSG,
    BVCONSOLEACTION_REPRINT,
    BVCONSOLEACTION_QUIT,
    BVCONSOLEACTION_PAUSE_DISCOVERY,
    BVCONSOLEACTION_RESUME_DISCOVERY,
    BVCONSOLEACTION_BLOCKHOST
};

struct ParsingResult
{
    BVConsoleActionType type;
    std::optional<int> num;
};

struct ConsoleActionS
{
    BVConsoleActionType type;
};

/* BVTerminal
   BVTerminal allows to set certain terminal options
   for a nicer output.
*/
class BVTerminal
{
private:
    class TerminalModeGuard
    {
    public:
        TerminalModeGuard()
        {
            if (tcgetattr(STDIN_FILENO, &original_) == -1)
            {
                throw std::runtime_error("tcgetattr failed");
            }

            termios canonical = original_;
            canonical.c_lflag |= ICANON;
            canonical.c_lflag |= ECHO;

            if (tcsetattr(STDIN_FILENO, TCSANOW, &canonical) == -1)
            {
                throw std::runtime_error("tcsetattr failed while enabling canonical mode");
            }
        }

        ~TerminalModeGuard()
        {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_);
        }

    private:
        termios original_{};
    };
    termios originalTerminal;
    termios currentTerminal;
    bool isInitialized{false};

    void EnsureInitialized(void) const
    {
        if (!isInitialized)
        {
            throw std::runtime_error("Terminal not initialized");
        }
    }


public:
    BVTerminal()
    {
        if (!::isatty(STDIN_FILENO))
        {
            throw std::runtime_error("BVTerminal: stdin is not a terminal");
        }

        if (::tcgetattr(STDIN_FILENO, &originalTerminal) != 0)
        {
            throw std::runtime_error("BVTerminal: tcgetattr failed");
        }
        currentTerminal = originalTerminal;
        isInitialized = true;
    }

    ~BVTerminal()
    {
        Restore();
    }

    enum class InputMode
    {
        Canonical,
        NonCanonical
    };

    struct Config
    {
        InputMode mode{InputMode::Canonical};
        bool echo{true};
        cc_t vmin{1};
        cc_t vtime{0};
    };

    BVTerminal(const BVTerminal&) = delete;
    BVTerminal& operator=(const BVTerminal&) = delete;

    void SetCanonicalMode(bool echo = true)
    {
        Config cfg;
        cfg.mode = InputMode::Canonical;
        cfg.echo = echo;
        Apply(cfg);
    }

    void SetNonCanonicalMode(bool echo = false, cc_t vmin = 1, cc_t vtime = 0)
    {
        Config cfg;
        cfg.mode = InputMode::NonCanonical;
        cfg.echo = echo;
        cfg.vmin = vmin;
        cfg.vtime = vtime;
        Apply(cfg);
    }

    void FlushInput() const
    {
        EnsureInitialized();
        if (::tcflush(STDIN_FILENO, TCIFLUSH) != 0)
        {
            throw std::runtime_error("BVTerminal: tcflush failed");
        }
    }

    void Apply(const Config& cfg)
    {
        EnsureInitialized();
        termios t = originalTerminal;
        if (cfg.mode == InputMode::Canonical)
        {
            t.c_lflag |= ICANON;
        }
        else
        {
            t.c_lflag &= ~ICANON;
            t.c_cc[VMIN] = cfg.vmin;
            t.c_cc[VTIME] = cfg.vtime;
        }
        if (cfg.echo)
        {
            t.c_lflag |= ECHO;
        }
        else
        {
            t.c_lflag &= ~ECHO;
        }
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0)
        {
            throw std::runtime_error("BVTerminal: tcsetattr failed");
        }
        currentTerminal = t;
    }

    void Restore()
    {
        if (isInitialized)
        {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &originalTerminal);
        }
    }

    char ReadChar() const
    {
        EnsureInitialized();

        char c = '\0';
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        // if (n < 0)
        // {
        //     throw std::runtime_error("BVTerminal: read failed");
        // }
        if (n == 0)
        {
            throw std::runtime_error("BVTerminal: EOF on stdin");
        }
        return c;
    }

    std::string ReadLine() const
    {
        EnsureInitialized();

        std::string line;
        if (!std::getline(std::cin, line))
        {
            throw std::runtime_error("BVTerminal: getline failed");
        }
        return line;
    }

    std::string GetStringFromSTDIN(const std::string& prompt)
    {
        EnsureInitialized();
        TerminalModeGuard guard{};
        std::cout << prompt;
        std::cout.flush();
        std::string s;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::getline(std::cin, s);
        return s;
    }

    std::string PromptLine(const std::string& prompt)
    {
        EnsureInitialized();
        TerminalModeGuard guard{};
        std::cout << prompt;
        std::cout.flush();
        if (std::cin.peek() == '\n')
        {
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
        std::string s;
        if (!std::getline(std::cin, s))
        {
            throw std::runtime_error("BVTerminal: getline failed");
        }
        return s;
    }
};

class BVApp_ConsoleClient : public BVApp,
                            public BVComponent,
                            public BVLoggable
{
private:
    std::mutex stdoutMutex; // mutex for internal worker threads, in this case printing.
    std::thread stdinThread; // worker thread? I don't think this is needed
    BVTerminal terminal{};

    // Event-driven console UI: stdin is read through the io_context (see Run()),
    // so keystrokes and incoming network messages are handled on the same thread.
    enum class UIState { MainMenu, Chat, FileOffer };
    UIState uiState{UIState::MainMenu};
    UIState priorUiState{UIState::MainMenu}; // state to return to after a file offer
    std::string activeChatService{}; // peer we are chatting with in UIState::Chat
    std::string inputLine{};         // message line being typed
    std::string lastNotification{};  // latest incoming message shown when not in its chat
    char readCh{0};                  // single-byte buffer for async stdin reads
    std::optional<boost::asio::posix::stream_descriptor> stdinDescriptor{};

    // Pending incoming file offer awaiting a y/n decision (UIState::FileOffer).
    struct PendingOffer { uint32_t key{}; std::string sender; std::string fname; std::uint64_t size{}; };
    std::optional<PendingOffer> pendingOffer{};
    std::uint64_t recvBytes{0};      // bytes received in the current incoming transfer

    void StartStdinRead(void);       // queue the next async read of one key
    void OnKey(char c);              // route a key to the active UI state
    void HandleMainMenuKey(char key);
    void HandleChatKey(char c);
    void HandleFileOfferKey(char c); // y/n decision while UIState::FileOffer
    void EnterChat(int idx);         // open chat with the idx-th known node
    void SendChatLine(const std::string& serviceName, const std::string& line);
    void Render(void);               // redraw whatever the current state shows
    void RenderChat(void);
    void RenderFileOffer(void);      // draw the Accept/Reject prompt

protected:
    BVNode ResolveServiceToEndpoint(const std::string& hosttarget, const std::string& serviceName, const int port) override;

public:
    BVApp_ConsoleClient(const BVServiceData _thisMachineServiceData,
                        std::shared_ptr<threadsafe_queue<BVMessage>> _outMbx,
                        std::shared_ptr<threadsafe_queue<BVMessage>> _inMbx,
                        boost::asio::io_context& _ioContext);

    void Run(void) override;

    // -------------------------------------------------------
    BVStatus HandlePublishedServices(std::unique_ptr<std::any> dp) override;
    BVStatus HandleResolvedServices(std::unique_ptr<std::any> dp) override;
    BVStatus HandleServiceDeregistration(std::unique_ptr<std::any>) override;
    BVStatus HandleMessageIncoming(std::unique_ptr<std::any>) override;
    // New file-offer handler (not part of the BVApp base). Virtual so the GUI
    // client can override the CLI's auto-accept with a user prompt.
    virtual BVStatus HandleFileOffer(std::unique_ptr<std::any>);
    BVStatus HandleFileTransferBegin(std::unique_ptr<std::any>) override;
    BVStatus HandleFileChunkSent(std::unique_ptr<std::any>) override;
    BVStatus HandleFileTransferEnd(std::unique_ptr<std::any>) override;
    // -------------------------------------------------------

    BVStatus ReadMessages(void);
    BVStatus PrintMessages(void);
    BVStatus PrintServices(void);
    void PrintNewServicesNotification(void);
    void PrintAll(void);
    inline void ClearScreen(void);
    std::optional<ParsingResult> ParseConsoleActionFromKey(char key);

    std::unique_ptr<BVTCPMessage<BVChatMessagePayload>> ConstructChatMessageFromInput(
        const std::string& inputString);//, const NodeID nodeID);

    // -------------------------------------------------------
    BVStatus OnStart(std::unique_ptr<std::any>) override;
    BVStatus OnResume(std::unique_ptr<std::any>) override;
    BVStatus OnShutdown(std::unique_ptr<std::any>) override;
    BVStatus OnRestart(std::unique_ptr<std::any>) override;
    BVStatus OnPause(std::unique_ptr<std::any>) override;
    // -------------------------------------------------------

    ~BVApp_ConsoleClient() {}
};
