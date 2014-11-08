function [ arap ] = comparap( V, V2, R, N, W )
  % Tao Du
  % Nov 8, 2014
  
  % Given the old and new vertices, the rotation matrices, neighborhood and
  % weight, computes the arap energy.
  % The format of R is a (# vertices x 3) x 3 matrix. It is generated by
  % 'stacking' the rotation matrix of each vertex together.

  % Get the nonzero edges.
  [I, J, ~] = find(N);
  
  % Get the number of edges.
  enum = length(I);
  
  % Initialize arap energy.
  arap = 0.0;
  
  % Loop over all the edges.
  for e = 1 : enum
    % Get the indices of the first and second edges.
    i = I(e);
    j = J(e);
    
    % Get the weight of the edge.
    w = W(i, j);
    
    % Get the old and new vertices.
    voi = V(i, :);
    voj = V(j, :);
    vni = V2(i, :);
    vnj = V2(j, :);
    
    % Get the rotation matrix.
    base = (e - 1) * 3;
    r = R(base + 1 : base + 3, :);
    
    % Add this edge into arap energy.
    arap = arap + compedge(w, vni, vnj, r, voi, voj);
  end
end

