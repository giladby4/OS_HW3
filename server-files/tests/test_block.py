from signal import SIGINT
from time import sleep
import pytest

from server import Server, server_port
from definitions import DYNAMIC_OUTPUT_CONTENT, SERVER_CONNECTION_OUTPUT
from utils import spawn_clients, generate_dynamic_headers, validate_out, validate_response_full, validate_response_full_with_dispatch
from requests_futures.sessions import FuturesSession

def test_sanity(server_port):
    with Server("./server", server_port, 1, 1, "block") as server:
        sleep(0.1)
        print("in")
        with FuturesSession() as session1:
            future1 = session1.get(f"http://localhost:{server_port}/output.cgi?1")
            sleep(0.1)
            print("inin")
            with FuturesSession() as session2:
                print("ininin")

                future2 = session2.get(f"http://localhost:{server_port}/output.cgi?1")
                print("inin")

                response = future2.result()
                print("inin")

                expected_headers = generate_dynamic_headers(123, 2, 0, 2)
                expected = DYNAMIC_OUTPUT_CONTENT.format(
                    seconds="1.0")
                print0("ininin")
                validate_response_full(response, expected_headers, expected)
            response = future1.result()
            expected_headers = generate_dynamic_headers(123, 1, 0, 1)
            expected = DYNAMIC_OUTPUT_CONTENT.format(
                seconds="1.0")
            validate_response_full(response, expected_headers, expected)
        server.send_signal(SIGINT)
        out, err = server.communicate()
        expected = SERVER_CONNECTION_OUTPUT.format(
            filename=r"/output.cgi\?1") * 2
        print(out)
        #validate_out(out, err, expected)


@pytest.mark.parametrize("threads, queue, amount, dispatches",
                         [
                             (2, 4, 4, [0,0,0.8,0.9]),
                             (2, 4, 8, [0,0,0.8,0.9,1.8,1.5,2.4,1.8]),
                             (4, 4, 8, [0,0,0,0,0.6,0.2,0.2,0.2,0.2]),
                             (4, 8, 8, [0,0,0,0,0.6,0.7,0.8,0.9]),
                             (4, 8, 10, [0,0,0,0,0.6,0.7,0.8,0.9,1.6,1.7]),
                         ])
def test_load(threads, queue, amount, dispatches, server_port):
    with Server("./server", server_port, threads, queue, "block") as server:
        sleep(0.1)
        clients = spawn_clients(amount, server_port)
        for i in range(amount):
            response = clients[i][1].result()
            clients[i][0].close()
            expected = DYNAMIC_OUTPUT_CONTENT.format(seconds=f"1.{i:0<1}")
            expected_headers = generate_dynamic_headers(123, (i // threads) + 1, 0, (i // threads) + 1)
            validate_response_full_with_dispatch(response, expected_headers, expected, dispatches[i])
        server.send_signal(SIGINT)
        out, err = server.communicate()
        expected = "^" + ''.join([SERVER_CONNECTION_OUTPUT.format(
            filename=rf"/output.cgi\?1.{i}") for i in range(amount)])
        validate_out(out, err, expected)

test_sanity(8080)