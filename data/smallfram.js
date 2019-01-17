
var domElement = function(selector) {
   this.selector = selector || null;
   this.element = null;
};

domElement.prototype.init = function() {
  switch (this.selector[0]) {
    case ‘<’:
      var matches = this.selector.match(/<([\w-]*)>/);
      if (matches === null || matches === undefined) {
        throw ‘Invalid Selector / Node’;
        return false;
      }
      var nodeName = matches[0].replace(‘<’, ‘’).replace(‘>’, ‘’);
      this.element = document.createElement(nodeName);
      break;
    default:
      this.element = document.querySelector(this.selector);
  }
};

domElement.prototype.val = function(newVal) {
   return (newVal !== undefined ? this.element.value = newVal : this.element.value);
};

domElement.prototype.append = function(html) {
   this.element.innerHTML = this.element.innerHTML + html;
};

domElement.prototype.prepend = function(html) {
   this.element.innerHTML = html + this.element.innerHTML;
};

domElement.prototype.html = function(html) {
   if (html === undefined) {
     return this.element.innerHTML;
   }
   this.element.innerHTML = html;
};

$ = function(selector) {
 var el = new domElement(selector);
 el.init();
 return el;
}
