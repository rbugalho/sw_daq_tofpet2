#include "AsyncWriter.hpp"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <algorithm>

using namespace PETSYS;
using namespace std;

DataWriter::DataWriter(const std::string& filename, bool acqStdMode) {
	if (filename == "/dev/null") {
		// Intentionally set fd to -1 when writing to /dev/null
		fd = -1;
		return;
	}

	fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC| O_DIRECT, 0644);

	if (fd < 0) {
		fprintf(stderr,"ERROR at '%s':%d open('%s') failed (%d)", __FILE__, __LINE__, filename.c_str(), errno);
		exit(-1);
	}

	if (io_setup(N_BUFFERS, &ctx) != 0){
		fprintf(stderr,"ERROR at '%s':%d io_setup failed (%d)", __FILE__, __LINE__, errno);
		exit(-1);
	}

	for (int i = 0; i < N_BUFFERS; i++) {
        int ret = posix_memalign(&buffers[i].data, BUFFER_SIZE, BUFFER_SIZE);
        if (ret != 0) {
		fprintf(stderr,"ERROR at '%s':%d posix_memalign alocation failed (%d)", __FILE__, __LINE__, errno);
		exit(-1);
        }
        memset(buffers[i].data, 0, BUFFER_SIZE);
	buffers[i].used = 0;
	memset(&buffers[i].cb, 0, sizeof(struct iocb));
    }
}

DataWriter::~DataWriter() {
	if(fd == -1) return;

	// Use a normal write to write any data in the last buffer
	// but we still need to pad it to IO_BLOCK_SIZE
	auto &currentBuffer = buffers[currentBufferIndex];
	auto write_size = (currentBuffer.used / IO_BLOCK_SIZE) * IO_BLOCK_SIZE;
	auto tail_size = currentBuffer.used - write_size;
	if(tail_size != 0) write_size += IO_BLOCK_SIZE;

	auto r = pwrite(fd, currentBuffer.data, write_size, global_offset);
	if(r == -1) {
		fprintf(stderr,"ERROR at '%s':%d pwrite failed (%d)", __FILE__, __LINE__, errno);
		exit(-1);
	}
	else if (r != write_size) {
		fprintf(stderr, "ERROR at '%s':%d pwrite (..., %ld, ...) returned %ld\n", __FILE__, __LINE__, write_size, r);
	}
	// Finally truncate the file to the correct size
	r = ftruncate(fd, global_offset + currentBuffer.used);
	if(r != 0) {
		fprintf(stderr, "ERROR at '%s':%d ftruncate failed (%d)", __FILE__, __LINE__, errno);
		exit(-1);
	}

	io_destroy(ctx);
	close(fd);
}

void DataWriter::appendData(void *buf, size_t count)
{
	if(fd == -1) return;

	size_t written = 0;
	while (written < count) {
		auto &currentBuffer = buffers[currentBufferIndex];
		auto bufferFree = BUFFER_SIZE - currentBuffer.used;
		auto copySize = min(count - written, bufferFree);
		memcpy((char *)currentBuffer.data + currentBuffer.used, (char *)buf+written, copySize);
		currentBuffer.used += copySize;
		written += copySize;

		if(currentBuffer.used == BUFFER_SIZE) {
			// This will also move currentBufferIndex to the next buffer
			submittCurrentBuffer();
		}
	}
}


long long DataWriter::getCurrentPosition()
{
	if(fd == -1) return 0;

	return global_offset + buffers[currentBufferIndex].used;
}


long long DataWriter::getCurrentPositionFromFile()
{
	if(fd == -1) return 0;


	off_t currentFilePos = lseek(fd, 0, SEEK_CUR);
	return (long long) currentFilePos;
}


void DataWriter::completeAllBuffers()
{
	if(fd == -1) return;

	if(currentBufferIndex == 0) return;

	// Wait for all pending writes to complete
	struct io_event events[currentBufferIndex];
	int completed = io_getevents(ctx, currentBufferIndex, currentBufferIndex, events, NULL);
	if(completed != currentBufferIndex) {
		fprintf(stderr, "ERROR at '%s':%d %d != %d", __FILE__, __LINE__, completed, currentBufferIndex);
		exit(-1);
	}

	currentBufferIndex = 0;
	for(auto i = 0; i < N_BUFFERS; i++) buffers[i].used = 0;
}

void DataWriter::submittCurrentBuffer()
{
	if(fd == -1) return;

	Buffer& currentBuffer = buffers[currentBufferIndex];
	assert(currentBuffer.used % IO_BLOCK_SIZE == 0);
	assert(global_offset % IO_BLOCK_SIZE == 0);

	struct iocb &cb = currentBuffer.cb;
	struct iocb* cbs[1] = {&cb};

	io_prep_pwrite(&cb, this->fd, currentBuffer.data, BUFFER_SIZE, global_offset);
	if(io_submit(ctx, 1, cbs) != 1) {
		fprintf(stderr, "ERROR at '%s':%d io_submit failed (%d)", __FILE__, __LINE__, errno);
		exit(-1);
	}

	global_offset += BUFFER_SIZE;
	currentBufferIndex += 1;

	if(currentBufferIndex == N_BUFFERS) {
		// All buffers have been submitted, complete them before proceeding
		completeAllBuffers();
	}
}

void DataWriter::writeHeader(unsigned long long fileCreationDAQTime, double daqSynchronizationEpoch, long systemFrequency, char *mode, int triggerID) {
	bool qdcMode = (strcmp(mode, "qdc") == 0);
	uint64_t header[8];
	for(int i = 0; i < 8; i++)
		header[i] = 0;
	header[0] |= uint32_t(systemFrequency);
	header[0] |= (qdcMode ? 0x1UL : 0x0UL) << 32;
	memcpy(header+1, &daqSynchronizationEpoch, sizeof(double));
	if (triggerID != -1) { header[2] = 0x8000 + triggerID; }
	if (strcmp(mode, "mixed") == 0) { header[3] = 0x1UL; }
	header[4] = fileCreationDAQTime;

	appendData((void *)&header, sizeof(uint64_t)*8);
}
