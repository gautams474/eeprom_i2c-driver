#include <linux/fs.h>	   // Inode and File types
#include <linux/i2c-dev.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <linux/errno.h>
#include <linux/unistd.h>

#define MY_IOCTL 'G'
#define FLASHGETS _IOR(MY_IOCTL, 0, int)     // busy status is read 
#define FLASHGETP _IOR(MY_IOCTL, 1, int)   // gets current address 
#define FLASHSETP _IOW(MY_IOCTL, 2, int)   // sets address
#define FLASHERASE _IO(MY_IOCTL, 3)          // erases memory
	
#define count 20       // no. of pages to be written
#define erase_c  512   // no. of erased pages to be displayed
#define r_count 30     // no. of pages to be read

extern int errno;
void fill_data_buf(char*,int);  // function to fill buffer for writing, defined after main

int main(int argc, char* argv)
{
	int i2c_fd;
	int page  = 0;
	int ret = 0;
	char data[64*count];         // writing buffer
	char rdata1[64*erase_c];
    char rdata2[64*r_count];     // reading buffer 
	int i,j=-1;
    int busyf; 

	printf("user starts\n");
	fill_data_buf(data,count);
	
	sleep(2);
    /*   open device */

	i2c_fd = open("/dev/i2c_flash", O_RDWR);
	if(i2c_fd < 0)
	{
		printf("Error: could not open i2c device.\n");
		return -1;
	}

	sleep(1);

	/* Do Flash Erase */
 
	ret = ioctl(i2c_fd,FLASHERASE);
	if(ret < 0)
	{
		printf("Error: ioctl failed.\n");
		return -1;
    } 
	printf("\n Flash erase over \n");
	sleep(2);

	/* Read Erased Chip */

	printf("\n reading erased data \n");
    ret=-1;
	while(ret<0)
	{
    	ret = read(i2c_fd,rdata1, erase_c);
		if(ret < 0)
			printf("Error: coud not read. \t %s\n" , strerror(errno));
		sleep(1);
	}
	
	sleep(4);

	/* Print Erased Data in chip */

    for(i=0;i<erase_c;i++)
		{
			printf("page :%d \t %.*s \n",(page+i),64,(rdata1 + (i*64)));
			usleep(10*1000);
		}
    sleep(5);
		
	/* Do FlashGetS */

	busyf = ioctl(i2c_fd,FLASHGETS,0);
	if(busyf < 0)
	{
		printf("Error: ioctl failed.\n");
		return -1;
    } 
	if( busyf == 1)
    	printf("\n EEPROM is busy \n");
	else 
		printf("\n EEPROM is NOT busy \n");  
    sleep(2);

	/* Set Page to write from*/
    page = 502; 
    ret = ioctl(i2c_fd,FLASHSETP,(unsigned long)page);
    sleep(2);
	
	/* write Data from buffer*/

	ret=-1;
	printf("\n start writing \n");
	while(ret<0)
	{
		ret = write(i2c_fd, data, count);
		if(ret < 0)
			printf("Error: could not write. \t %s\n" , strerror(errno));
			usleep(100*1000);
	}
	printf("\n write finished\n");
    sleep(5);

	/* Set page to read from */
    page =495;
	ret = ioctl(i2c_fd,FLASHSETP,(unsigned int)page);
    if(ret < 0)
	{
		printf("Error: ioctl failed.\n");
		return -1;
    }

    /* Read data that was written */
	ret = -1 ;
	while(ret < 0)
	{
		ret = read(i2c_fd, rdata2, r_count);
		if(ret < 0)
			printf("Error: coud not read. \t %s\n" , strerror(errno));
		sleep(1);
	}
	
	printf("\n read finished\n");
	sleep(2);

	/* Get current page address */

    page = ioctl(i2c_fd,FLASHGETP,(unsigned int)page);
    if(ret < 0)
	{
		printf("Error: ioctl flashgetp failed.\n");
		return -1;
    } 
    printf("\n current page :%d\n", page);
    
	/* Print data that was read */
 
	for(i =0; i< r_count*64; i++)
	{
		if(i%64 ==0)
		{
			int p;
			j++;
			p = page+j-r_count;
			if(p < 0)
				p = 512 + p;
			printf("\npage :%d \t", p);
	    }
		printf("%c",rdata2[i]);
	} 
    sleep(2);
    close(i2c_fd); 
	return 0;
}

void fill_data_buf(char* buf, int c)
{
	int i,j;

	for(i=0; i<c; i++)
	{
		for(j=0; j<64; j++) 		
			buf[(i*64)+j] = 48+j;
	}
	
}
