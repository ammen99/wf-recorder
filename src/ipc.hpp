#ifndef IPC_HPP
#define IPC_HPP

/* Receives pixels over FD, writes a frame
 * each time it has read format_width * width * height pixels */
void write_loop(uint32_t width, uint32_t height, int format_width, int fd);

#endif /* end of include guard: IPC_HPP */
