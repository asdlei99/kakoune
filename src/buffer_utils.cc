#include "buffer_utils.hh"

#include "buffer_manager.hh"
#include "event_manager.hh"
#include "file.hh"
#include "selection.hh"

#include <unistd.h>

#if defined(__APPLE__)
#define st_mtim st_mtimespec
#endif


namespace Kakoune
{

ColumnCount get_column(const Buffer& buffer,
                       ColumnCount tabstop, BufferCoord coord)
{
    auto line = buffer[coord.line];
    auto col = 0_col;
    for (auto it = line.begin();
         it != line.end() and coord.column > (int)(it - line.begin()); )
    {
        if (*it == '\t')
        {
            col = (col / tabstop + 1) * tabstop;
            ++it;
        }
        else
            col += codepoint_width(utf8::read_codepoint(it, line.end()));
    }
    return col;
}

ColumnCount column_length(const Buffer& buffer, ColumnCount tabstop, LineCount line)
{
    return get_column(buffer, tabstop, BufferCoord{line, ByteCount{INT_MAX}});
}

ByteCount get_byte_to_column(const Buffer& buffer, ColumnCount tabstop, DisplayCoord coord)
{
    auto line = buffer[coord.line];
    auto col = 0_col;
    auto it = line.begin();
    while (it != line.end() and coord.column > col)
    {
        if (*it == '\t')
        {
            col = (col / tabstop + 1) * tabstop;
            if (col > coord.column) // the target column was in the tab
                break;
            ++it;
        }
        else
        {
            auto next = it;
            col += codepoint_width(utf8::read_codepoint(next, line.end()));
            if (col > coord.column) // the target column was in the char
                break;
            it = next;
        }
    }
    return (int)(it - line.begin());
}

Buffer* open_file_buffer(StringView filename, Buffer::Flags flags)
{
    MappedFile file_data{parse_filename(filename)};
    return BufferManager::instance().create_buffer(
        filename.str(), Buffer::Flags::File | flags, file_data, file_data.st.st_mtim);
}

Buffer* open_or_create_file_buffer(StringView filename, Buffer::Flags flags)
{
    auto& buffer_manager = BufferManager::instance();
    auto path = parse_filename(filename);
    if (file_exists(path))
    {
        MappedFile file_data{path};
        return buffer_manager.create_buffer(filename.str(), Buffer::Flags::File | flags,
                                            file_data, file_data.st.st_mtim);
    }
    return buffer_manager.create_buffer(
        filename.str(), Buffer::Flags::File | Buffer::Flags::New,
        {}, InvalidTime);
}

void reload_file_buffer(Buffer& buffer)
{
    kak_assert(buffer.flags() & Buffer::Flags::File);
    MappedFile file_data{buffer.name()};
    buffer.reload(file_data, file_data.st.st_mtim);
    buffer.flags() &= ~Buffer::Flags::New;
}

Buffer* create_fifo_buffer(String name, int fd, Buffer::Flags flags, bool scroll)
{
    static ValueId fifo_watcher_id = get_free_value_id();

    auto& buffer_manager = BufferManager::instance();
    Buffer* buffer = buffer_manager.get_buffer_ifp(name);
    if (buffer)
    {
        buffer->flags() |= Buffer::Flags::NoUndo | flags;
        buffer->reload({}, InvalidTime);
    }
    else
        buffer = buffer_manager.create_buffer(
            std::move(name), flags | Buffer::Flags::Fifo | Buffer::Flags::NoUndo);

    struct FifoWatcher : FDWatcher
    {
        FifoWatcher(int fd, Buffer& buffer, bool scroll)
            : FDWatcher(fd, FdEvents::Read,
                        [](FDWatcher& watcher, FdEvents, EventMode mode) {
                            if (mode == EventMode::Normal)
                                static_cast<FifoWatcher&>(watcher).read_fifo();
                        }),
              m_buffer(buffer), m_scroll(scroll)
        {}

        ~FifoWatcher()
        {
            kak_assert(m_buffer.flags() & Buffer::Flags::Fifo);
            close_fd();
            m_buffer.run_hook_in_own_context(Hook::BufCloseFifo, "");
            m_buffer.flags() &= ~(Buffer::Flags::Fifo | Buffer::Flags::NoUndo);
        }

        void read_fifo() const
        {
            kak_assert(m_buffer.flags() & Buffer::Flags::Fifo);

            constexpr size_t buffer_size = 2048;
            // if we read data slower than it arrives in the fifo, limiting the
            // iteration number allows us to go back go back to the event loop and
            // handle other events sources (such as input)
            constexpr size_t max_loop = 16;
            bool closed = false;
            size_t loop = 0;
            char data[buffer_size];
            BufferCoord insert_coord = m_buffer.back_coord();
            const int fifo = fd();
            do
            {
                const ssize_t count = ::read(fifo, data, buffer_size);
                if (count <= 0)
                {
                    closed = true;
                    break;
                }

                auto pos = m_buffer.back_coord();
                const bool prevent_scrolling = pos == BufferCoord{0,0} and not m_scroll;
                if (prevent_scrolling)
                    pos = m_buffer.next(pos);

                m_buffer.insert(pos, StringView(data, data+count));

                if (prevent_scrolling)
                {
                    m_buffer.erase({0,0}, m_buffer.next({0,0}));
                    // in the other case, the buffer will have automatically
                    // inserted a \n to guarantee its invariant.
                    if (data[count-1] == '\n')
                        m_buffer.insert(m_buffer.end_coord(), "\n");
                }
            }
            while (++loop < max_loop  and fd_readable(fifo));

            if (insert_coord != m_buffer.back_coord())
                m_buffer.run_hook_in_own_context(
                    Hook::BufReadFifo,
                    selection_to_string(ColumnType::Byte, m_buffer, {insert_coord, m_buffer.back_coord()}));

            if (closed)
                m_buffer.values().erase(fifo_watcher_id); // will delete this
        }

        Buffer& m_buffer;
        bool m_scroll;
    };

    buffer->values()[fifo_watcher_id] = Value(std::make_unique<FifoWatcher>(fd, *buffer, scroll));
    buffer->flags() = flags | Buffer::Flags::Fifo | Buffer::Flags::NoUndo;
    buffer->run_hook_in_own_context(Hook::BufOpenFifo, buffer->name());

    return buffer;
}

void write_to_debug_buffer(StringView str)
{
    if (not BufferManager::has_instance())
    {
        write(2, str);
        write(2, "\n");
        return;
    }

    constexpr StringView debug_buffer_name = "*debug*";
    // Try to ensure we keep an empty line at the end of the debug buffer
    // where the user can put its cursor to scroll with new messages
    const bool eol_back = not str.empty() and str.back() == '\n';
    if (Buffer* buffer = BufferManager::instance().get_buffer_ifp(debug_buffer_name))
    {
        buffer->flags() &= ~Buffer::Flags::ReadOnly;
        auto restore = on_scope_end([buffer] { buffer->flags() |= Buffer::Flags::ReadOnly; });

        buffer->insert(buffer->back_coord(), eol_back ? str : str + "\n");
    }
    else
    {
        String line = str + (eol_back ? "\n" : "\n\n");
        BufferManager::instance().create_buffer(
            debug_buffer_name.str(), Buffer::Flags::NoUndo | Buffer::Flags::Debug | Buffer::Flags::ReadOnly,
            line, InvalidTime);
    }
}

}
