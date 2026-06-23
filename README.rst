Configuration Tool for 3Dconnexion CadMouse
-------------------------------------------

Since 3Dconnexion does not have any open-source tool to change firmware
settings for their CadMouse device, Martin Herkt, mia-0, and most recently kevenwyld,
have reverse engineered its configuration protocol from USB captures of the Windows
driver.

Compilation
===========

This needs hidapi

Linux
~~~~~

``cc cadmousectl.c -o cadmousectl $(pkg-config --cflags --libs hidapi-hidraw)``

Windows, macOS, \*BSD, …
~~~~~~~~~~~~~~~~~~~~~~~~

Windows builds are availabe on the GitHub Releases page.

``cc cadmousectl.c -o cadmousectl $(pkg-config --cflags --libs hidapi)``


Usage
=====

This tool needs read/write access to the mouse's ``hidraw`` device. The
supplied udev rules tag it with ``uaccess``, which automatically grants the
logged-in local user access (over USB and Bluetooth), so you do not need to
run ``cadmousectl`` as root. The mouse loses its settings each time it is
disconnected. Therefore, you will probably want to set up ``cadmousectl`` to
run automatically. Each rules file needs to be modified as necessary (the path
to ``cadmousectl`` and the parameters).

The CadMouse Pro (``256f:c656``) and CadMouse Pro Wireless (``256f:c654``)
use a single configuration report for all settings, so pass every option you
want in **one** invocation — running ``cadmousectl`` again for a different
setting resets the previously set ones back to defaults.

CadMouse Pro Wireless also supports Bluetooth — settings are applied the same
way, but polling rate will be limited to ~89Hz.

Linux
~~~~~

Copy the ``rules/linux/99-cadmouse.rules`` file into ``/etc/udev/rules.d``
and reload ``udevd``.

FreeBSD
~~~~~~~

Copy the ``rules/freebsd/cadmouse.conf`` file into ``/usr/local/etc/devd``
and reload ``devd``.

NOTE: These rules have not been updated to support wireless or Bluetooth
as I have no FreeBSD machine to test with.

Parameters
~~~~~~~~~~

cadmousectl [-[lprsS] value] [--keepalive]

+--------------+---------------------------------------------------------+
| Option       | Effect                                                  |
+==============+=========================================================+
| -l           | Enable (non-zero) or disable (zero) lift-off detection. |
+--------------+---------------------------------------------------------+
| -p           | Set polling rate (125, 250, 500, 1000).                 |
|              | Bluetooth: limited to ~89Hz regardless of setting.      |
+--------------+---------------------------------------------------------+
| -r           | Remap buttons. Format is real_button:assigned_button.   |
|              |                                                         |
|              | Values for real_button:                                 |
|              |     left, right, middle, wheel, forward, backward, rm   |
|              |                                                         |
|              | Values for assigned_button:                             |
|              |     left, right, middle, backward, forward, rm, extra   |
|              |                                                         |
|              | .. note::                                               |
|              |     The extra button was discovered by accident.        |
|              |     Using this, you can assign an additional button to  |
|              |     the wheel click. It will have id 11 on X11.         |
+--------------+---------------------------------------------------------+
| -s           | Set speed (1-164).                                      |
|              |                                                         |
|              | Cannot be used with -d                                  |
+--------------+---------------------------------------------------------+
| -d           | Set speed in DPI (50-8200).                             |
|              |                                                         |
|              | Cannot be used with -s                                  |
+--------------+---------------------------------------------------------+
| -S           | Set Smart Scroll mode. There are two additional modes   |
|              | which the Windows GUI does not expose.                  |
|              |                                                         |
|              | 0                                                       |
|              |     off                                                 |
|              | 1                                                       |
|              |     normal                                              |
|              | 2                                                       |
|              |     slow                                                |
|              | 3                                                       |
|              |     accelerated scrolling                               |
|              |                                                         |
|              | Accelerated scrolling mode is different from the other  |
|              | modes in that it does not simulate a flywheel but       |
|              | instead sends more scroll wheel clicks the faster you   |
|              | scroll.                                                 |
+--------------+---------------------------------------------------------+
| -k,          | Keep device alive (prevents USB disconnect).            |
| --keepalive  | Use with systemd for persistent keepalive:              |
|              | ``sudo cp systemd/cadmouse-keepalive.service``          |
|              | ``/etc/systemd/system/ && sudo systemctl``              |
|              | ``enable --now cadmouse-keepalive``                     |
|              |                                                         |
|              | see https://askubuntu.com/a/1542182                     |
|              | When the bluetooth mouse is connected via a usb cable   |
|              | it stops responding, you must listen to the event       |
|              | interface or it will stop reporting events you          | 
|              | can do this with cat, or with the keepalive argument    |
+--------------+---------------------------------------------------------+

License
=======

This software is available under the terms of the ISC license as it appears
in each source file.
