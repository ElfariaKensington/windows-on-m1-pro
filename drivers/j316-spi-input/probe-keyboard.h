/* SPDX-License-Identifier: MIT
 * 90-byte report descriptor measured from the live J316s SPI keyboard via
 * IOHIDDevice ReportDescriptor. Used only by the resource-free Windows probe;
 * the production source obtains its descriptor from the physical keyboard.
 */
static UCHAR keyboard_descriptor[] = {
    5,1,9,6,161,1,133,1,5,7,25,224,41,231,21,0,37,1,117,1,149,8,129,2,
    149,1,117,8,129,1,149,5,117,1,5,8,25,1,41,5,145,2,149,1,117,3,145,1,
    149,6,117,8,21,0,38,255,0,5,7,25,0,41,255,129,0,5,12,117,1,149,1,
    9,184,21,0,37,1,129,2,5,255,9,3,117,7,149,1,129,2,192
};
