
#ifndef __ARCH_MOCK_IO_HPP__
#define __ARCH_MOCK_IO_HPP__

#include "containers/segmented_vector.hpp"
#include "utils2.hpp"
#include "arch/random_delay.hpp"
#include <stdlib.h>

struct mock_iocallback_t {
    virtual void on_io_complete() = 0;
    virtual ~mock_iocallback_t() {}
};

#define DEFAULT_DISK_ACCOUNT NULL

template<class inner_io_config_t>
class mock_file_t
{
    int mode;

public:
    enum mode_t {
        mode_read = 1 << 0,
        mode_write = 1 << 1,
        mode_create = 1 << 2
    };

    struct account_t {
        account_t(UNUSED mock_file_t *f, UNUSED int p, UNUSED int outstanding_requests_limit = UNLIMITED_OUTSTANDING_REQUESTS) { }
    };

protected:
    mock_file_t(const char *path, int mode, const typename inner_io_config_t::io_backend_t io_backend = (typename inner_io_config_t::io_backend_t)-1, const int batch_factor = DEFAULT_IO_BATCH_FACTOR)
        : mode(mode)
    {
        int mode2 = 0;
        if (mode & mode_read) mode2 |= inner_io_config_t::file_t::mode_read;
        // We always enable writing because the mock layer does
        // truncation on exit
        mode2 |= inner_io_config_t::file_t::mode_write;
        if (mode & mode_create) mode2 |= inner_io_config_t::file_t::mode_create;

        if (io_backend == (typename inner_io_config_t::io_backend_t)-1)
            // Use the defaults of the underlying implementation
            inner_file = new typename inner_io_config_t::nondirect_file_t(path, mode2);
        else
            inner_file = new typename inner_io_config_t::nondirect_file_t(path, mode2, io_backend, batch_factor);

        if (inner_file->is_block_device()) {
            fail_due_to_user_error(
                "Using mock_file_t with a block device is a really bad idea because it "
                "reads the entire contents of the underlying file into memory, which could be "
                "a lot for a block device.");
        }
        
        set_size(inner_file->get_size());
        for (unsigned i = 0; i < get_size() / DEVICE_BLOCK_SIZE; i++) {
            inner_file->read_blocking(i*DEVICE_BLOCK_SIZE, DEVICE_BLOCK_SIZE, blocks[i].data);
        }
    }

    bool exists() {
        return inner_file->exists();
    }
    
    bool is_block_device() {
        return false;
    }
    
    uint64_t get_size() {
        return blocks.get_size() * DEVICE_BLOCK_SIZE;
    }
    
    void set_size(size_t size) {
#ifndef NDEBUG
        {
            bool modcmp = size % DEVICE_BLOCK_SIZE == 0;
            rassert(modcmp);
        }
#endif
        blocks.set_size(size / DEVICE_BLOCK_SIZE);
    }
    
    void set_size_at_least(size_t size) {
        if (get_size() < size) {
            size_t actual_size = size + randint(10) * DEVICE_BLOCK_SIZE;
            set_size(actual_size);
        }
    }
    
    /* These always return 'false'; the reason they return bool instead of void
    is for consistency with other asynchronous-callback methods */
    bool read_async(size_t offset, size_t length, void *buf, UNUSED account_t *account, mock_iocallback_t *cb) {
        rassert(mode & mode_read);
        read_blocking(offset, length, buf);
        random_delay(cb, &mock_iocallback_t::on_io_complete);
        return false;
    }
    
    bool write_async(size_t offset, size_t length, const void *buf, UNUSED account_t *account, mock_iocallback_t *cb) {
        rassert(mode & mode_write);
        write_blocking(offset, length, buf);
        random_delay(cb, &mock_iocallback_t::on_io_complete);
        return false;
    }
    
    void read_blocking(size_t offset, size_t length, void *buf) {
        rassert(mode & mode_read);
        verify(offset, length, buf);
        for (unsigned i = 0; i < length / DEVICE_BLOCK_SIZE; i += 1) {
            memcpy((char*)buf + i*DEVICE_BLOCK_SIZE, blocks[offset/DEVICE_BLOCK_SIZE + i].data, DEVICE_BLOCK_SIZE);
        }
    }
    
    void write_blocking(size_t offset, size_t length, const void *buf) {
        rassert(mode & mode_write);
        verify(offset, length, buf);
        for (unsigned i = 0; i < length / DEVICE_BLOCK_SIZE; i += 1) {
            memcpy(blocks[offset/DEVICE_BLOCK_SIZE + i].data, (char*)buf + i*DEVICE_BLOCK_SIZE, DEVICE_BLOCK_SIZE);
        }
    }
    
    ~mock_file_t() {
        if (mode & mode_write) {
            if(exists()) {
                inner_file->set_size(get_size());
            }
            // This is horrible. We write every single block of the file, sequentially, in a
            // blocking fashion. There must be a better way to do this...
            for (unsigned i = 0; i < get_size() / DEVICE_BLOCK_SIZE; i++) {
                inner_file->write_blocking(i*DEVICE_BLOCK_SIZE, DEVICE_BLOCK_SIZE, blocks[i].data);
            }
        }
        
        delete inner_file;
    }

private:
    typename inner_io_config_t::nondirect_file_t *inner_file;
    
    struct block_t {
        char *data;
        
        block_t() {
            data = (char*)malloc_aligned(DEVICE_BLOCK_SIZE, DEVICE_BLOCK_SIZE);
            
            // Initialize to either random data or zeroes, choosing at random
            char d = randint(2) ? 0 : randint(0x100);
            memset(data, d, DEVICE_BLOCK_SIZE);
        }
        
        ~block_t() {
            free((void*)data);
        }
    };
    segmented_vector_t<block_t, 10*GIGABYTE/DEVICE_BLOCK_SIZE> blocks;
    
    void verify(UNUSED size_t offset, UNUSED size_t length, UNUSED const void *buf) {
#ifndef NDEBUG
        rassert(buf);
        rassert(offset + length <= get_size());
        bool modbuf = (intptr_t)buf % DEVICE_BLOCK_SIZE == 0;
        rassert(modbuf);
        bool modoff = offset % DEVICE_BLOCK_SIZE == 0;
        rassert(modoff);
        bool modlength = length % DEVICE_BLOCK_SIZE == 0;
        rassert(modlength);
#endif
    }

    DISABLE_COPYING(mock_file_t);
};

template <class inner_io_config_t>
class mock_direct_file_t : public mock_file_t<inner_io_config_t> {
public:
    using mock_file_t<inner_io_config_t>::exists;
    using mock_file_t<inner_io_config_t>::is_block_device;
    using mock_file_t<inner_io_config_t>::get_size;
    using mock_file_t<inner_io_config_t>::set_size;
    using mock_file_t<inner_io_config_t>::set_size_at_least;
    using mock_file_t<inner_io_config_t>::read_async;
    using mock_file_t<inner_io_config_t>::write_async;
    using mock_file_t<inner_io_config_t>::read_blocking;
    using mock_file_t<inner_io_config_t>::write_blocking;

    mock_direct_file_t(const char *path, int mode, const typename inner_io_config_t::io_backend_t io_backend = (typename inner_io_config_t::io_backend_t)-1, const int batch_factor = DEFAULT_IO_BATCH_FACTOR) : mock_file_t<inner_io_config_t>(path, mode, io_backend, batch_factor) { }

private:
    DISABLE_COPYING(mock_direct_file_t);
};

template <class inner_io_config_t>
class mock_nondirect_file_t : private mock_file_t<inner_io_config_t> {
public:
    using mock_file_t<inner_io_config_t>::exists;
    using mock_file_t<inner_io_config_t>::is_block_device;
    using mock_file_t<inner_io_config_t>::get_size;
    using mock_file_t<inner_io_config_t>::set_size;
    using mock_file_t<inner_io_config_t>::set_size_at_least;
    using mock_file_t<inner_io_config_t>::read_async;
    using mock_file_t<inner_io_config_t>::write_async;
    using mock_file_t<inner_io_config_t>::read_blocking;
    using mock_file_t<inner_io_config_t>::write_blocking;

    mock_nondirect_file_t(const char *path, int mode, const typename inner_io_config_t::io_backend_t io_backend = (typename inner_io_config_t::io_backend_t)-1, const int batch_factor = DEFAULT_IO_BATCH_FACTOR) : mock_file_t<inner_io_config_t>(path, mode, io_backend, batch_factor) { }

private:
    DISABLE_COPYING(mock_nondirect_file_t);
};


#endif /* __ARCH_MOCK_IO_HPP__ */
