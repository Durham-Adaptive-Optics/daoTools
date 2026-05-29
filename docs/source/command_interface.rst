Command Interface
=================

``daoCommandIfce`` provides a ZMQ REQ/REP command client for sending protobuf-encoded commands to DAO RTC processes and receiving their replies.

Source: ``src/python/daoCommandIfce.py``


Overview
--------

DAO processes that expose a command server communicate over TCP using ``daoCommand.proto`` messages. ``daoCommandIfce`` handles connection management, serialisation, and timeout/reconnect logic transparently.

.. code-block:: python

    from daoCommandIfce import daoCommandIfce
    import daoCommand_pb2

    ifce = daoCommandIfce(ip='192.168.1.10', port=5000)

    # Build a command message
    cmd = daoCommand_pb2.CommandMessage()
    cmd.function = daoCommand_pb2.SET_GAIN
    cmd.payload  = '0.3'

    # Send and receive
    status, payload = ifce.sendCommand(cmd)

    # Check result
    ifce.check_status(status, payload)


Constructor
-----------

.. code-block:: python

    daoCommandIfce(ip, port, name='sendCommands', timeout=0.5)

.. list-table::
   :widths: 20 15 65
   :header-rows: 1

   * - Parameter
     - Default
     - Description

   * - ``ip``
     - —
     - IP address of the remote command server.

   * - ``port``
     - —
     - TCP port of the remote command server.

   * - ``name``
     - ``'sendCommands'``
     - Logger name used for this interface instance.

   * - ``timeout``
     - ``0.5``
     - Seconds to wait for a reply before timing out and resetting the socket.


Methods
-------

sendCommand
~~~~~~~~~~~

.. code-block:: python

    status, payload = ifce.sendCommand(Command)

Serialises ``Command`` (a ``daoCommand_pb2.CommandMessage``) and sends it over the ZMQ REQ socket. Polls for a reply up to ``timeout`` seconds.

Returns a tuple:

.. list-table::
   :widths: 20 80
   :header-rows: 1

   * - ``status``
     - Meaning

   * - ``0``
     - Success — ``payload`` contains the response string.

   * - ``1``
     - Command failed — see ``payload`` for error details.

   * - ``2``
     - Timeout — socket was reset and reconnected automatically.

On timeout the socket is closed and a fresh ``zmq.REQ`` socket is connected, ready for the next call.

check_status
~~~~~~~~~~~~

.. code-block:: python

    ifce.check_status(status, payload)

Logs the outcome of a command at the appropriate level:

- Status ``0`` → ``INFO: Command Successful``
- Status ``1`` → ``ERROR: Command failed``
- Status ``2`` → ``ERROR: Timeout``
- Other → ``ERROR: Unknown response``


Error Handling
--------------

The interface uses a non-blocking ``zmq.EAGAIN`` polling loop so the calling thread is never permanently blocked. After ``timeout`` seconds with no reply:

1. The existing socket is closed.
2. A new ``zmq.REQ`` socket is created and connected to the same endpoint.
3. ``(2, "Timeout")`` is returned to the caller.

This allows the caller to retry or escalate without restarting the interface object.


Example: Loop Gain Control
--------------------------

.. code-block:: python

    from daoCommandIfce import daoCommandIfce
    import daoCommand_pb2

    # Connect to the loop controller's command server
    loop_cmd = daoCommandIfce(ip='localhost', port=5001, timeout=1.0)

    # Build SET_GAIN command
    cmd = daoCommand_pb2.CommandMessage()
    cmd.function = daoCommand_pb2.SET_GAIN
    cmd.payload  = '0.5'

    status, reply = loop_cmd.sendCommand(cmd)
    loop_cmd.check_status(status, reply)

    # Open the loop
    cmd.function = daoCommand_pb2.OPEN_LOOP
    cmd.payload  = ''
    status, reply = loop_cmd.sendCommand(cmd)
    loop_cmd.check_status(status, reply)
