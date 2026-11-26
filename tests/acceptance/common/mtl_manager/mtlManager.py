import logging
import os
import time

from mfd_connect.exceptions import ConnectionCalledProcessError
from mtl_engine.const import MTL_MANAGER_EXE

logger = logging.getLogger(__name__)

MANAGER2_SOCK_PATH = "/run/imtl/manager2.sock"
MANAGER_READY_TIMEOUT_S = 5.0
MANAGER_STOP_WAIT_S = 2.0


class MtlManager:
    """
    Class to manage the lifecycle of the MtlManager process on a remote host.

    Attributes:
        host: Host object containing a .connection attribute for remote command execution.
        mtl_manager_process: The running MtlManager process object (if started).
    """

    def __init__(self, host, mtl_path=""):
        """
        Initialize the MtlManager with a host object.
        :param host: Host object with a .connection attribute.
        :param mtl_path: Root path of MTL repo (used to resolve binary path).
        """
        self.host = host
        if mtl_path:
            exe = os.path.join(mtl_path, MTL_MANAGER_EXE)
        else:
            exe = MTL_MANAGER_EXE
        self.cmd = f"sudo {exe}"
        self.mtl_manager_process = None

    def _is_socket_ready(self, connection):
        probe_cmd = (
            'python3 -c "'
            "import socket, sys; "
            f"s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); "
            "s.settimeout(0.5); "
            f"s.connect('{MANAGER2_SOCK_PATH}'); "
            's.sendall(b\'{\\"method\\":\\"org.varlink.service.GetInfo\\",\\"parameters\\":{}}\\x00\'); '
            "data = s.recv(4096); "
            "s.close(); "
            "sys.exit(0 if b'MtlManager 2' in data else 1)"
            '"'
        )
        res = connection.execute_command(probe_cmd, expected_return_codes=None)
        return res.return_code == 0

    def start(self):
        """
        Starts the MtlManager process on the remote host using sudo and waits
        for /run/imtl/manager2.sock readiness via Varlink GetInfo.
        Returns True if started successfully, False otherwise.
        """
        if not self.mtl_manager_process or not self.mtl_manager_process.running:
            connection = self.host.connection
            try:
                logger.info(f"Running command on host {self.host.name}: {self.cmd}")
                self.mtl_manager_process = connection.start_process(
                    self.cmd, stderr_to_stdout=True
                )

                deadline = time.monotonic() + MANAGER_READY_TIMEOUT_S
                while time.monotonic() < deadline:
                    if not self.mtl_manager_process.running:
                        err = self.mtl_manager_process.stderr_text
                        logger.error(
                            f"MtlManager failed to start. Error output:\n{err}"
                        )
                        return False
                    if self._is_socket_ready(connection):
                        logger.info(
                            f"MtlManager started with PID {self.mtl_manager_process.pid} "
                            f"and ready on {MANAGER2_SOCK_PATH}."
                        )
                        return True
                    time.sleep(0.1)

                logger.error(
                    f"MtlManager did not become ready on {MANAGER2_SOCK_PATH} "
                    f"within {MANAGER_READY_TIMEOUT_S} s."
                )
                return False
            except ConnectionCalledProcessError as e:
                logger.error(f"Failed to start MtlManager: {e}")
                return False
        else:
            logger.info("MtlManager is already running.")
            return True

    def stop(self):
        """
        Stops the MtlManager process on the remote host.
        Sends SIGTERM first for clean non-destructive shutdown, then falls back
        to pkill if the process is still running.
        """
        connection = self.host.connection
        try:
            logger.info("Stopping MtlManager with SIGTERM...")
            connection.execute_command(
                "sudo pkill -TERM -x MtlManager", expected_return_codes=None
            )
            deadline = time.monotonic() + MANAGER_STOP_WAIT_S
            while time.monotonic() < deadline:
                chk = connection.execute_command(
                    "pgrep -x MtlManager", expected_return_codes=None
                )
                if chk.return_code != 0:
                    logger.info("MtlManager stopped cleanly via SIGTERM.")
                    return
                time.sleep(0.1)
        except ConnectionCalledProcessError as e:
            logger.warning(f"MtlManager SIGTERM failed ({e}), falling back to pkill")

        try:
            logger.info("Stopping MtlManager using sudo pkill MtlManager (fallback)...")
            connection.execute_command(
                "sudo pkill MtlManager", expected_return_codes=None
            )
            logger.info("MtlManager stopped (via pkill).")
        except ConnectionCalledProcessError as e:
            logger.error(f"Failed to stop MtlManager via pkill: {e}")
