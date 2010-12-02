This program uses CUSE (Character Devices in Userspace), introduced in kernel 2.6.31.
To build it you'll need the g++ compiler and the development libraries for:
- boost
- libxml2
- fuse

These instructions assume you got it built OK.


AS ROOT:

Load the cuse module (or add 'cuse' to /etc/modules to load at startup):

 modprobe cuse
 
Move the 'real' joystick device somewhere where x-plane won't find it:

 mv /dev/input/js0 /dev/input/realj0
 
Make a new node for the 'virtual' joystick (check /proc/devices to pick a major
number that's unused - here I chose 249)

 mknod /dev/input/js0 c 249 0

Now, AS YOUR NORMAL USER:
 
Run stickshift (I still use the debug -d option so that I can see when things
go wrong; you probably should too to begin with):
    
 ./stickshift -d -I /dev/input/realj0 -M 249 -m 0 -c x52pro.xml \
              --calibrated=cal_out.xml
