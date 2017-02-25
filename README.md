# IMR surround view utest application

Demo application demonstrates surround view with pre-defined viewpoints on Renesas boards.

## Build
Project uses cmake tool to build an application.
Example for H3

```
. /opt/poky/2.0.2/environment-setup-aarch64-poky-linux 
mkdir build
cd build
cmake -DIMR_TARGET_PLATFORM=GEN3 ../
make

```
Optional values for IMR_TARGET_PLATFORM are GEN3, GEN2.

## Run
To run application 
```
usage: utest-imr-sv [options]
Options and arguments:
-d  : Debug log level (default: 1)
-v  : Paths to 4 VIN camera devices(default: /dev/video0,/dev/video1,/dev/video2,/dev/video3 )
-r  : Paths to 5 imr devices (default: /dev/video8,/dev/video9,/dev/video10,/dev/video11,/dev/video12)
-f  : Video format input (available options: uyvy,yuyv,nv12,nv16
-o  :  Desired Weston display output number 0, 1,.., N
-w  : VIN camera capture width (default: 1280)
-h  : VIN camera capture height (default: 800)
-W  : VSP width render output (default: 1280)
-H  : VSP height render output (default: 720)
-X  : Car png file width
-Y  : Car png file height
-n  : Number of buffers for VIN
-s  : Number of steps for model positions (default: 8:32:8)
-m  : Model PNG picture prefix path (default: ./data/model)
-M  : Mesh file path
-S  : Car shadow rectangle
-g  : Sphere gain
-b  : Background color
```
Example of usage:

```
./imr-wl -f uyvy -v /dev/video0,/dev/video1,/dev/video2,/dev/video3 -w 1280 -h 800 -W 1920 -H 1080 \
	-r /dev/video4,/dev/video5,/dev/video6,/dev/video7,/dev/video4,/dev/video5,/dev/video6,/dev/video7 -m ./data/model \
       -M meshFull.obj -X 1920 -Y 1080 -S -0.30:-0.10:0.30:0.10 -g 1.0 -s 8:32:8
```

Example of generation png files with car (avalaible only for Gen3):

```
./gen -w <width> -h <height> -c <color> -o <path to store> -s <positions> -m <car object> \
-l <car length> -S <shadow rectangle> -d <debug>
./gen -w 1920 -h 1080  -c 0x404040FF -o ./data/model -s 8:32:8 -m Car.obj -l 1.0  -S -0.2:-0.10:0.2:0.10
```


