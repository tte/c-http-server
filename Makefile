default: compile && ./http_server

compile:
	gcc -o http_server http_server.c 
