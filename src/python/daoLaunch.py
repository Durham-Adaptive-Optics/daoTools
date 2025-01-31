#!/usr/bin/env python
import subprocess
import logging
from daoLog import daoLog
import time
import select

def manage_process(action: str, tmuxname=None, user=None, machine=None, processExe=None, processArgs=None, workingDir='./'):
    """
    This function manages the starting and killing of a process in a tmux session.

    Parameters:
    - action (str): The action to perform, either 'start' or 'kill'.
    - tmuxname (str): The name of the tmux session.
    - user (str): The user to run the commands as.
    - machine (str): The machine to run the commands on.
    - processExe (str): The executable file to run.
    - processArgs (str): The arguments to pass to the executable file.

    Returns:
    - None
    """
    if action not in ('launch', 'kill'):
        raise ValueError("Invalid action value. It must be either 'start' or 'kill'.")
    if action == 'launch':
        commands = [f'tmux send-keys -t {tmuxname} C-c 2> /dev/null',
                    f'sleep 0.1',
                    f'tmux new -d -s {tmuxname}',
                    f'tmux send-keys -t {tmuxname} "cd {workingDir}" C-M',
                    f'echo "Executing {processExe} {processArgs} in tmux session {tmuxname}"',
                    f'tmux send-keys -t {tmuxname} "{processExe} {processArgs}" C-M',
                    f'echo "Done starting {processExe}."']
    else:
        commands = [f'tmux send-keys -t {tmuxname} C-c 2> /dev/null',
                    f'sleep 0.1',
                    f'tmux send-keys -t {tmuxname} "exit" Enter 2> /dev/null',
                    f'sleep 0.1',
                    f'tmux kill-session -t {tmuxname}']
    send_commands(commands, user, machine)



def check_tmux_session(tmuxname=None, machine=None, user=None) -> bool:
    """
    Check if a tmux session exists on the specified machine and user.

    Parameters:
    machine (str): The machine to check the tmux session on.
    user (str): The user to check the tmux session for.
    tmuxname (str): The name of the tmux session to check for.

    Returns:
    bool: True if the tmux session exists, False otherwise.
    """
    if machine is not None and user is not None:
        commands = ["ssh", f"{user}@{machine}"]
    else:
        commands = []
    commands.extend(["tmux", "list-sessions"])
    result = subprocess.run(commands, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    sessions = [line.split(" ")[0].strip(":") for line in result.stdout.decode().strip().split("\n")]
    if tmuxname in sessions:
        return True
    else:
        return False



def list_tmux_sessions(machine: str, user: str) -> list:
    """
    This function lists all the tmux sessions on a remote machine.
    :param machine: The hostname or IP address of the remote machine.
    :param user: The username used to connect to the remote machine.
    :return: A list of all the tmux sessions on the remote machine.
    """
    if machine is not None and user is not None:
        commands = ["ssh", f"{user}@{machine}"]
    else:
        commands = []
    commands.extend(["tmux", "list-sessions"])
    result = subprocess.run(commands, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    sessions = result.stdout.decode().strip().split("\n")
    return sessions


def send_commands(commands, user=None, machine=None, timeout=0.1):
    """
    Sends a list of commands to the specified machine through ssh or runs locally if no machine is specified. 
    
    Parameters:
    commands (list): List of commands to be executed
    user (str, optional): The username to log in to the machine. Defaults to None.
    machine (str, optional): The target machine name. Defaults to None.
    timeout (float, optional): The time in seconds to wait for a response from the target machine. Defaults to 0.1.
    
    Returns:
    str: The output from the executed commands
    
    Raises:
    TypeError: If `commands` is not a list, `user` is not a string (if provided), or `machine` is not a string (if provided).
    
    Notes:
    - If either user and machine are not specified, the commands are run locally using the sh shell.
    - The function uses the logging module to log the debug of the function.
    """
    log = logging.getLogger(__name__)
    # Validate input parameters
    if not isinstance(commands, list):
        raise TypeError("commands must be a list")
    if user is not None and not isinstance(user, str):
        raise TypeError("user must be a string")
    if machine is not None and not isinstance(machine, str):
        raise TypeError("machine must be a string")

    log.debug(f"send_commands({commands}, {user},{machine}")
    output=""
    try:
        if user is None or machine is None:
            process = subprocess.Popen(["sh"],stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True)
        else:
            process = subprocess.Popen([
                                "ssh",
                                "-o UserKnownHostsFile=/dev/null",
                                "-o StrictHostKeyChecking=no",
                                f"{user}@{machine}"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for command in commands:
            log.debug(command)
            process.stdin.write(command.encode() + b'\n')
            process.stdin.flush()
            start = time.time()
            while process.poll() is None and time.time() - start < timeout:
                r, w, x = select.select([process.stdout], [], [], 0)
                if r:
                    output += process.stdout.readline().decode()

        process.stdin.close()
        process.stdout.close()
        process.stderr.close()
        process.wait()

    except Exception as e:
        log.error(e)
        raise
    return output
    


def launch_xterm(machine=None, user=None, tmux=None, args=None):
    """
    Launches an xterm terminal with ssh session to the specified machine
    and attaches to the specified tmux session.
    If machine is not specified, opens a local xterm.
    If user is not specified, uses current user.
    If tmux is not specified, does not attach to any tmux session.
    """
    command = ["xterm"]
    if machine is not None:
        command.append("-e")
        if user is not None:
            string = f"ssh {user}@{machine} -t"
        else:
            string = ""
        
    if tmux is not None:
        string = f"{string} tmux a -t {tmux}"

    string = (f"{string}; exec bash")
    command.append(string)
    print(f"runnning: {command}")
    subprocess.run(command)


if __name__=="__main__":
    logger = daoLog(__name__)

    tmuxname = 'test'
    processExe = "ls"
    processArgs = '-la'
    commands = [f'tmux send-keys -t {tmuxname} C-c 2> /dev/null',
                f'sleep 0.1',
                f'echo "Executing {processExe} {processArgs} in tmux session {tmuxname}"',
                f'tmux new -d -s {tmuxname}',
                f'sleep 0.1',
                f"tmux send-keys -t {tmuxname} '{processExe} {processArgs}' C-M",
                f'echo "Done starting {processExe}."']
    

    A = send_commands(commands, user=None, machine=None)
    print(A)
    
