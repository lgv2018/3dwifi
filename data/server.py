import SimpleHTTPServer
import SocketServer
import urlparse
import cgi

PORT = 8000

class FakeHttpSerialGcode(SimpleHTTPServer.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        SimpleHTTPServer.SimpleHTTPRequestHandler.__init__(self, *args, **kwargs)

    def do_POST(self):
        #self._set_headers()
        self.send_response(200)
        self.end_headers()

        ctype, pdict = cgi.parse_header(self.headers['Content-Type'])
        if ctype == 'multipart/form-data':
            #pdict['boundary'] = bytes(pdict['boundary'], 'utf-8')
            fields = cgi.parse_multipart(self.rfile, pdict)
            filename = fields.get('filename')[0].decode('utf-8')
            print "Filename: " + filename
            with open('test.txt', 'w') as outfile:
                outfile.write(fields.get('file')[0])
        return

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

        elif bits.path == '/gcode' and args['cmd'][0] == 'M105':
            self.send_response(200)
            self.send_header('Content-type', 'text/txt')
            self.end_headers()
            self.wfile.write('ok T:68.76 /190.00 B:29.47 /60.00 @:127 B@:127\n')

        elif bits.path == '/gcode' and args['cmd'][0] == 'M27':
            self.send_response(200)
            self.send_header('Content-type', 'text/txt')
            self.end_headers()
            self.wfile.write('SD printing byte 50/100.\nok\n')

        elif bits.path == '/gcode' and args['cmd'][0] == 'M27 C':
            self.send_response(200)
            self.send_header('Content-type', 'text/txt')
            self.end_headers()
            self.wfile.write('Current file: teste.gco\nok\n')

        elif bits.path == '/gcode' and args['cmd'][0] == 'M31':
            self.send_response(200)
            self.send_header('Content-type', 'text/txt')
            self.end_headers()
            self.wfile.write('echo:Print time: 1d 23h 49m 37s\nok\n')

        elif bits.path == '/gcode':
            self.send_response(200)
            self.send_header('Content-type', 'text/txt')
            self.end_headers()
            self.wfile.write('ok\n')

        elif bits.path == '/msg':
            self.send_response(200)
            self.send_header('Content-type', 'text/txt')
            self.end_headers()
            self.wfile.write(' SD printing byte 50/100.\nT:68.76 /190.00 B:29.47 /60.00 @:127 B@:127\nT:68.76 /190.00 B:29.47 /60.00 @:127 B@:127\n')

        else:
            return SimpleHTTPServer.SimpleHTTPRequestHandler.do_GET(self)

Handler = FakeHttpSerialGcode

httpd = SocketServer.TCPServer(("", PORT), Handler)

print "serving at port", PORT
httpd.serve_forever()
