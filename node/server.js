var http = require('http');
var url = require('url');
var fs = require('fs');
var qs = require('querystring');
var request = require('request');
var cheerio = require('cheerio');
var zlib = require('zlib');
var util = require('util');
var Transform = require('stream').Transform;

util.inherits(Deflate, Transform);
function Deflate(options) {
    if (!(this instanceof Parser))
        return new Parser(options);
    Transform.call(this, options);
}
Deflate.prototype._transform = function(data, encoding, done) {
  var req = request(options);
  req.on('response', function (res) {
    if (res.statusCode !== 200) throw new Error('Status not 200')
 
    var encoding = res.headers['content-encoding']
    if (encoding == 'gzip') {
      this.push(zlib.createGunzip());
    } else if (encoding == 'deflate') {
      this.push(zlib.createInflate());
    } else {
      this.push(res);
    }
  });
  req.on('error', function(err) {
    throw err;
  });
}

util.inherits(Parser, Transform);
function Parser(options) {
    if (!(this instanceof Parser))
        return new Parser(options);
    Transform.call(this, options);
    this._blob = [];
    this._chunk = 0;
}
Parser.prototype._transform = function(data, encoding, done) {
    var complete = -1;
    //console.log('got chunk ' + this._chunk + ', ' + data.length + ' bytes, finished = ' + data.toString().indexOf("</html>"));
    this._chunk = this._chunk + 1;
    if (data.toString().indexOf("</html>") != -1) {
        complete = 1;
    }
    this._blob.push(data);
    
    if (complete == 1) {
        var buffer = Buffer.concat(this._blob).toString();
        var $ = cheerio.load(buffer);
        var links = [];
        $('li[rel="track"]', '#mainContainer').each(function(i, e) {
            var link = $(this).attr('data-ca');
            links[i] = link;
        });

        links = links.reverse().filter(function (e, i, links) {
            return links.indexOf(e, i+1) === -1;
        }).reverse();

        this.push('<html>');
        for (var i = 0; i < links.length; ++i) {
            var imgsrc = '<img src="' + links[i] + '"/>\n';
            this.push(imgsrc);
            //console.log(i + ' ' + links[i]);
            //this.push('<br>' + links[i] + '\n</br>');
        }
        this.push('</html>');
    }
    done();
}

function onRequest(req, res) {
	var url_parts = url.parse(req.url, true);

	if (req.method === 'GET') {
		req.on('data', function(data) { res.end(' data event: ' + data);});
		if (url_parts.pathname == '/') {
			fs.readFile('./form.html', function(error,data){
				res.end(data);
			});
		} else {
			res.writeHead(301, {
				'Location': '/'
			});
			res.end();
		}
	} else if (req.method === 'POST') {
		req.on('data', function (data) {
			var unescaped_data = unescape(data);
			var mypost = qs.parse(unescaped_data);
			var user_url = mypost.url;
	
			if (user_url == null) {
				res.writeHead(301, {
					'Location': '/'
				});
			} else {
				if (user_url != null) {
					//console.log('Requesting page ' + user_url);
					
                    // var example1 = 'https://play.spotify.com/user/umphreys/playlist/6hBEw1ggOPkRZy9pBjibsA';
                    // var example2 = 'http://open.spotify.com/user/umphreys/playlist/6hBEw1ggOPkRZy9pBjibsA';
                    // var example3 = 'https://embed.spotify.com/?uri=spotify:user:umphreys:playlist:6hBEw1ggOPkRZy9pBjibsA';
                    // var example4 = 'spotify:user:umphreys:playlist:6hBEw1ggOPkRZy9pBjibsA';
                    
                    var spotify_url = '';
                    if (user_url.indexOf("open.spotify.com") > -1) {
                        var re = /https?:\/\/open.spotify.com\/user\/(\w+)\/playlist\/(.*)/i;
                        var found = user_url.match(re);
                        if (found != null) {
                            spotify_url = 'https://embed.spotify.com/?uri=spotify:user:' + found[1] + ':playlist:' + found[2];
                            //console.log('[1] ' + spotify_url);
                        }
                    } else if (user_url.indexOf("play.spotify.com") > -1) {
                        var re = /https?:\/\/play.spotify.com\/user\/(\w+)\/playlist\/(.*)/i;
                        var found = user_url.match(re);
                        if (found != null) {
                            spotify_url = 'https://embed.spotify.com/?uri=spotify:user:' + found[1] + ':playlist:' + found[2];
                            //console.log('[2] ' + spotify_url);
                        }
                    } else if (user_url.indexOf("spotify:") > -1) {
                        var re = /spotify:user:(\w+):playlist:(.*)/i;
                        var found = user_url.match(re);
                        if (found != null) {
                            spotify_url = 'https://embed.spotify.com/?uri=spotify:user:' + found[1] + ':playlist:' + found[2];
                            //console.log('[3] ' + spotify_url);
                        }
                    }
                    
                    if (spotify_url == '') {
                        var html = '<html><p>Invalid Spotify URL</p>Please enter your input like one of the following:';
                        html += '<ul><li>http://open.spotify.com/user/umphreys/playlist/6hBEw1ggOPkRZy9pBjibsA</li>';
                        html += '<li>spotify:user:umphreys:playlist:6hBEw1ggOPkRZy9pBjibsA</li></ul></html>';
                        res.write(html);
                    } else {
                        var options = {
                            url: spotify_url,
                            //proxy: 'http://localhost:8888',
                            //strictSSL: false // needed for fiddler ssl decryption
                            headers: {
                                'User-Agent': 'Mozilla/5.0 (Windows NT 6.3; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/33.0.1750.117 Safari/537.36',
                                'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8',
                                //'Accept-Encoding': 'gzip,deflate,sdch',
                                'Accept-Language': 'en-US,en;q=0.8'
                            }
                        };
                        var parser = new Parser();
                        //res.write('<html>is this working?');
                        request.get(options).pipe(parser).pipe(res);
                        //res.write('</html>');
                        //res.end();
                    }
                }
			}
		});

		//req.on('end', function () { 
		//	res.end();
		//});        
	}
}

http.createServer(onRequest).listen(8889);
console.log("Server has started.");

