import SimpleHTTPServer
import SocketServer
import urlparse
import cgi

PORT = 8000

class FakeHttpSerialGcode(SimpleHTTPServer.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        SimpleHTTPServer.SimpleHTTPRequestHandler.__init__(self, *args, **kwargs)
    def do_GET(self):
        bits = urlparse.urlparse(self.path)
        print "bits: ", bits

        args = {}
        if bits.query != "":
            args = cgi.parse_qs(bits.query)

        print args

        # fake file list
        if bits.path == '/gcode' and args['cmd'][0] == 'M20':
            self.send_response(200)
            self.send_header('Content-type', 'text/txt')
            self.end_headers()
            self.wfile.write('Begin file list\n')
            self.wfile.write('/RC/F450/UPMODV~1.GCO 12345\n')
            self.wfile.write('/RC/F450/DOWNMO~1.GCO 123\n')
            self.wfile.write('/RC/SPACER~1/SPACER~1.GCO 523\n')
            self.wfile.write('/RC/CHEERS~1.GCO 134\n')
            self.wfile.write('End file list\n')
            self.wfile.write('ok \n')

        elif bits.path == '/gcode':
            self.send_response(200)
            self.send_header('Content-type', 'text/txt')
            self.end_headers()
            self.wfile.write('ok \n')

        else:
            return SimpleHTTPServer.SimpleHTTPRequestHandler.do_GET(self)

Handler = FakeHttpSerialGcode

httpd = SocketServer.TCPServer(("", PORT), Handler)

print "serving at port", PORT
httpd.serve_forever()
