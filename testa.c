#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
int main()
{
  
  int temp;
  int desc = open("/dev/lab0portB", O_RDONLY);
  read(desc, &temp, sizeof(int));
  close(desc);

}
